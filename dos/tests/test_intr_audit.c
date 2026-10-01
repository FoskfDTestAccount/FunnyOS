/*
 * An adversarial pass over the interrupt core: the stubs, the vector
 * table, the trap, the retry, and what a machine says about itself.
 *
 * ---------------------------------------------------------------------
 * Why this file exists next to test_intr.c
 *
 * The interrupt core is the one piece of M4 with no second pair of eyes
 * on it. Its author's evidence was a probe program, which is exactly the
 * evidence a project-wide rule here says does not count: the person who
 * wrote a thing cannot, by testing it, find out what they never thought
 * to check.
 *
 * So this suite is not a re-run of the author's tests. It came from the
 * reference material (docs/dos-refs.md), from the 8086 model, and from
 * reading the code looking for the places where the comment and the code
 * could disagree -- and it deliberately includes cases that pin
 * behaviour which is *wrong* but currently unobservable, so that fixing
 * it is a decision somebody makes rather than a change nobody notices.
 * Those cases say so in their names and comments.
 *
 * ---------------------------------------------------------------------
 * What is not covered, and why
 *
 * The run loop (vm86_run) is not in the tree yet, so nothing here
 * delivers a hardware interrupt the way the machine eventually will:
 * every pending-interrupt case calls vm86_interrupt() itself, and the
 * boundary logic the loop will apply is tested only as the predicate
 * vm86_interruptible() that the loop is supposed to consult. A case that
 * reads as "an interrupt was delivered" would be a case asserting
 * something that has not been written.
 *
 * The same goes for the frame a retry leaves behind: the loop is what
 * would refill the buffer the retry is waiting for, so what is pinned
 * here is the shape of the retry, not a whole blocking read.
 */
#include "harness.h"

#include <string.h>

#include <vm86/host.h>
#include <vm86/firmware.h>

/* ------------------------------------------------------------------ */
/* The stub, byte for byte                                             */
/* ------------------------------------------------------------------ */

/* Where a vector's stub should be: the table starts at F000:0000 and
 * every entry is four bytes, which is the same arithmetic ivt.c does with
 * different words. Written out here rather than called, because a test
 * that borrows the implementation's own helper cannot notice the helper
 * being wrong. */
#define STUB_AT(v)  (VM86_TRAP_LINEAR + (uint32_t)(v) * VM86_TRAP_STRIDE)

static void expect_stub(struct vm86_cpu *cpu, uint8_t vector, const char *what)
{
    uint32_t at = STUB_AT(vector);

    vm86_expect_mem8(what, cpu, at,      VM86_TRAP_OPCODE);
    vm86_expect_mem8(what, cpu, at + 1u, VM86_TRAP_MODRM);
    vm86_expect_mem8(what, cpu, at + 2u, vector);
    vm86_expect_mem8(what, cpu, at + 3u, VM86_TRAP_IRET);
}

/* ------------------------------------------------------------------ */
/* The vector table                                                    */
/* ------------------------------------------------------------------ */

static void test_every_vector_points_at_its_own_stub(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    for (unsigned v = 0; v < 256; v++) {
        uint32_t entry = (uint32_t)v * 4u;

        vm86_expect_mem16("the offset of a vector", cpu, entry,
                          (uint16_t)(v * VM86_TRAP_STRIDE));
        vm86_expect_mem16("the segment of a vector", cpu, entry + 2u,
                          VM86_TRAP_SEGMENT);
    }
}

static void test_every_stub_is_four_bytes_of_the_right_shape(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    /* Both ends of the table and a few in between, because an off-by-one
     * in the stride shows up at the ends rather than in the middle. */
    expect_stub(cpu, 0x00, "the first stub");
    expect_stub(cpu, 0x01, "the second stub");
    expect_stub(cpu, 0x10, "the video stub");
    expect_stub(cpu, 0xFE, "the second to last stub");
    expect_stub(cpu, 0xFF, "the last stub");
}

/*
 * The table is one kilobyte of stub area plus one kilobyte of vectors,
 * and it must not be anything else.
 *
 * Memory is pre-filled with a byte no stub or vector can produce, so this
 * is not "the table looks right" but "these bytes and no others were
 * written". That is the question the task asks -- whether the install
 * reaches past its own region -- and it is the kind of question that a
 * test asserting the region's contents cannot answer.
 */
static void test_install_ivt_writes_its_own_region_and_no_more(struct vm86_cpu *cpu)
{
    vm86_clear_services();

    memset(cpu->mem->ram, 0xA5, cpu->mem->size);

    vm86_install_ivt(cpu);

    unsigned stray = 0;
    uint32_t first_stray = 0;
    uint32_t last_stray = 0;

    for (uint32_t a = 0; a < cpu->mem->size; a++) {
        if (cpu->mem->ram[a] == 0xA5)
            continue;

        bool in_ivt   = a < 0x400u;
        bool in_stubs = a >= VM86_TRAP_LINEAR
                     && a <  VM86_TRAP_LINEAR + VM86_TRAP_BYTES;

        if (!in_ivt && !in_stubs) {
            if (stray == 0)
                first_stray = a;
            last_stray = a;
            stray++;
        }
    }

    vm86_expect_u16("bytes written outside the table", stray, 0);
    vm86_expect_u16("the first stray byte, if any", (uint16_t)first_stray, 0);
    vm86_expect_u16("the last stray byte, if any", (uint16_t)last_stray, 0);

    /* And the far end: the last four bytes belong to vector FF, and the
     * byte just past them belongs to nobody. */
    vm86_expect_u16("the last stub's opcode, written",
                    (uint16_t)cpu->mem->ram[VM86_TRAP_LINEAR + VM86_TRAP_BYTES - 4],
                    VM86_TRAP_OPCODE);
    vm86_expect_u16("the last stub's IRET, written",
                    (uint16_t)cpu->mem->ram[VM86_TRAP_LINEAR + VM86_TRAP_BYTES - 1],
                    VM86_TRAP_IRET);
    vm86_expect_u16("and one byte further on, still the fill",
                    (uint16_t)cpu->mem->ram[VM86_TRAP_LINEAR + VM86_TRAP_BYTES],
                    0xA5);
}

static void test_a_hooked_vector_stops_reading_as_a_stub(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    vm86_expect_bool("installed and therefore a stub",
                     vm86_vector_is_stub(cpu, 0x09), true);

    /* The guest takes the vector over. */
    vm86_mem_write16(cpu->mem, 0x09u * 4u, 0x1234u);
    vm86_mem_write16(cpu->mem, 0x09u * 4u + 2u, 0x5678u);

    vm86_expect_bool("hooked and therefore not a stub",
                     vm86_vector_is_stub(cpu, 0x09), false);

    /* Half a hook is still a hook. */
    vm86_mem_write16(cpu->mem, 0x09u * 4u, (uint16_t)(0x09u * VM86_TRAP_STRIDE));
    vm86_expect_bool("the offset put back but not the segment",
                     vm86_vector_is_stub(cpu, 0x09), false);

    vm86_mem_write16(cpu->mem, 0x09u * 4u + 2u, VM86_TRAP_SEGMENT);
    vm86_expect_bool("and now it reads as a stub again",
                     vm86_vector_is_stub(cpu, 0x09), true);
}

/*
 * A guest that stores the stub's own address is reported as not having
 * hooked the vector.
 *
 * This is the honest answer -- the question asked is "does this vector
 * still point at our stub", and it does -- but it is worth pinning
 * because it is surprising, and because the alternative reading ("the
 * guest wrote to the vector, so it meant to hook it") would need the
 * write to have been watched, which is the machinery this design
 * deliberately does not have. See M4-5.
 */
static void test_a_guest_that_stores_the_same_vector_reads_as_unhooked(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    vm86_mem_write16(cpu->mem, 0x00u * 4u, 0x0000u);
    vm86_mem_write16(cpu->mem, 0x00u * 4u + 2u, VM86_TRAP_SEGMENT);

    vm86_expect_bool("a guest storing what was already there",
                     vm86_vector_is_stub(cpu, 0x00), true);
}

/* ------------------------------------------------------------------ */
/* Reaching a service through INT, and coming back                     */
/* ------------------------------------------------------------------ */

struct counter {
    unsigned calls;
    uint16_t last_ax;
};

static void counting_service(struct vm86_cpu *cpu, void *ctx)
{
    struct counter *c = ctx;

    c->calls++;
    c->last_ax = cpu->ax;
    cpu->ax = 0x1234u;
}

/*
 * The whole chain, driven the way a guest drives it: INT reads the table,
 * the table points at the stub, the stub traps, the service runs, the
 * stub's IRET returns.
 *
 * Nothing in this test knows the trap exists -- it only sends an
 * instruction and looks at the answer -- which is the point. A suite that
 * called vm86_host_trap() directly would not notice the table pointing
 * somewhere else.
 */
static void test_int_reaches_the_service_and_returns(struct vm86_cpu *cpu)
{
    struct counter c = { 0, 0 };

    vm86_clear_services();
    vm86_install_ivt(cpu);
    vm86_register_service(0x21, counting_service, &c);

    /* mov ax, 0x00FF ; int 21h ; hlt */
    static const uint8_t code[] = { 0xB8, 0xFF, 0x00, 0xCD, 0x21, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF;

    enum vm86_result result = vm86_test_run(cpu, 100);

    vm86_expect_u16("the service ran", c.calls, 1);
    vm86_expect_u16("and saw the guest's AX on the way in", c.last_ax, 0x00FF);
    vm86_expect_bool("the machine halted after the call",
                     result == VM86_HALT, true);
    vm86_expect_u16("with the guest's registers back where it left them",
                    cpu->ax, 0x1234);
    vm86_expect_u16("and the guest's stack balanced",
                    cpu->sp, VM86_TEST_STACK_TOP);
}

/*
 * A vector nobody registered is not an error: the stub's IRET runs and
 * the program carries on. That is what firmware does for most of the
 * table, and a machine that stopped instead would break every program
 * that hooks a vector it never calls.
 */
static void test_an_unregistered_vector_is_harmless(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    static const uint8_t code[] = { 0xCD, 0x60, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 100);

    vm86_expect_bool("the machine reached the halt", result == VM86_HALT, true);
    vm86_expect_u16("one instruction past where it halted", cpu->ip, 0x0103);
    vm86_expect_u16("and the stack is where it started",
                    cpu->sp, VM86_TEST_STACK_TOP);
}

/*
 * A guest can reach the host without an INT, by executing the trap bytes
 * itself -- which host.h says is not a hole in the model, since a DOS
 * program could call the BIOS directly on real hardware too.
 *
 * The guest has to supply the frame the stub's IRET will unwind, because
 * there was no INT to push one. The frame here is built from outside the
 * guest so that the case does not depend on which push instructions this
 * interpreter has.
 */
static void test_a_guest_can_execute_the_trap_itself(struct vm86_cpu *cpu)
{
    struct counter c = { 0, 0 };

    vm86_clear_services();
    vm86_install_ivt(cpu);
    vm86_register_service(0x10, counting_service, &c);

    /* FE 38 10 at 0x0100 -- the trap, naming vector 10h -- then IRET, then
     * a halt at 0x0104. */
    static const uint8_t code[] = { 0xFE, 0x38, 0x10, 0xCF, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* The frame the guest would have pushed: return offset, return
     * segment, flags. */
    cpu->sp = 0xFFF0;
    vm86_mem_write16(cpu->mem, 0xFFF0, 0x0104u);
    vm86_mem_write16(cpu->mem, 0xFFF2, 0x0000u);
    vm86_mem_write16(cpu->mem, 0xFFF4, VM86_FLAG_ALWAYS_SET);

    enum vm86_result result = vm86_test_run(cpu, 100);

    vm86_expect_u16("the service ran without an INT", c.calls, 1);
    vm86_expect_bool("and the IRET returned to the guest's frame",
                     result == VM86_HALT, true);
    vm86_expect_u16("at the offset the guest pushed", cpu->ip, 0x0105);
    vm86_expect_u16("with the stack unwound", cpu->sp, 0xFFF6);
}

/* ------------------------------------------------------------------ */
/* What the trap records, and where a retry goes                       */
/* ------------------------------------------------------------------ */

/*
 * Where the trap says it began.
 *
 * host.h calls this "where the instruction being executed began" and
 * cpu.h says the run loop records it before the prefixes are consumed.
 * The trap overwrites whatever is there with its own answer, and its own
 * answer is "the byte two back from the service byte" -- which is the
 * stub's FE only when nothing sits between the two.
 *
 * Unprefixed and displacement-free -- which is what every stub this
 * firmware installs looks like -- the two agree, and that is this case.
 */
static void test_the_trap_records_its_own_start(struct vm86_cpu *cpu)
{
    struct counter c = { 0, 0 };

    vm86_clear_services();
    vm86_install_ivt(cpu);
    vm86_register_service(0x21, counting_service, &c);

    static const uint8_t code[] = { 0xCD, 0x21, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* Two steps, because the INT does not run the trap: the first step
     * pushes the frame and vectors, the second is the stub's FE 38. */
    vm86_step(cpu);
    vm86_step(cpu);

    vm86_expect_u16("the recorded start is the stub's first byte",
                    cpu->insn_ip, (uint16_t)(0x21u * VM86_TRAP_STRIDE));
}

/*
 * *** PINS A KNOWN DEFECT ***  -- see M4-intr-audit-report.md
 *
 * A segment override in front of the trap moves the instruction's real
 * start one byte earlier, and the trap does not know it. The value it
 * records lands on the FE rather than on the overridden prefix.
 *
 * Nothing in the tree can reach this today (no assembler emits FE 38 at
 * all, which is the whole reason the encoding was chosen), so the case is
 * here to make the arithmetic's limits mechanical rather than a claim in
 * a comment. If the arithmetic is ever made exact, this assertion must be
 * inverted -- and the test's name says which one is the defect.
 *
 * The retry consequence is *not* what makes this a defect: rewinding to
 * the FE re-runs the same trap, because the prefix was doing nothing to
 * an instruction that reads no memory. What it breaks is the field's
 * meaning for anything else that reads it.
 */
static void test_a_prefix_in_front_of_the_trap_is_not_accounted_for(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();

    /* F3 FE 38 10 CF, at F000:0080. */
    static const uint8_t stub[] = { 0xF3, 0xFE, 0x38, 0x10, 0xCF };

    for (unsigned i = 0; i < sizeof(stub); i++)
        vm86_mem_write8(cpu->mem, 0xF0080u + i, stub[i]);

    vm86_set_seg(cpu, VM86_CS, 0xF000u);
    cpu->ip = 0x0080u;

    vm86_step(cpu);

    vm86_expect_u16("the trap recorded the FE, not the prefix before it",
                    cpu->insn_ip, 0x0081u);
    vm86_expect_u16("and the instruction really did start here",
                    (uint16_t)(cpu->insn_ip - 1u), 0x0080u);
}

/*
 * Where a service that retries once ends up.
 *
 * Recorded from inside the service rather than after the step, because
 * the question is what the rewind target *was* -- reading the CPU later
 * would see wherever the machine went on to.
 */
struct retry_probe {
    unsigned calls;
    uint16_t ip_after_retry;
};

static void retry_probe_service(struct vm86_cpu *cpu, void *ctx)
{
    struct retry_probe *p = ctx;

    p->calls++;

    vm86_service_retry(cpu);

    p->ip_after_retry = cpu->ip;
}

/*
 * *** PINS A KNOWN DEFECT ***  -- see M4-intr-audit-report.md
 *
 * A memory-form ModRM carrying a displacement is the other way to put a
 * byte between the trap and its service byte, and this one *is* harmful:
 * what gets recorded is the ModRM byte, so a retry would rewind onto
 * 7F, which is `jg rel8`, and jump somewhere.
 *
 * The comment in trap.c describes this case as "a register-form ModRM
 * other than 38, where a displacement would sit between the opcode and
 * the service byte". A register-form ModRM (mod = 11) never carries a
 * displacement; the case that does is a *memory*-form one with mod = 01
 * or mod = 10, or mod = 00 with r/m = 6, which takes a full sixteen-bit
 * address. FE 7F 05 is mod = 01.
 */
static void test_a_displacement_between_the_trap_and_its_service_byte(
        struct vm86_cpu *cpu)
{
    struct retry_probe probe;
    probe.calls = 0;
    probe.ip_after_retry = 0;

    vm86_clear_services();

    /* FE 7F 05 10 CF, at F000:0080: the ModRM is mod=01, reg=111, r/m=111,
     * so a displacement byte sits between it and the service byte. */
    static const uint8_t stub[] = { 0xFE, 0x7F, 0x05, 0x10, 0xCF };

    for (unsigned i = 0; i < sizeof(stub); i++)
        vm86_mem_write8(cpu->mem, 0xF0080u + i, stub[i]);

    vm86_register_service(0x10, retry_probe_service, &probe);

    vm86_set_seg(cpu, VM86_CS, 0xF000u);
    cpu->ip = 0x0080u;

    vm86_step(cpu);

    vm86_expect_u16("the service ran", probe.calls, 1);
    vm86_expect_u16("the retry rewound onto the ModRM byte, not the opcode",
                    probe.ip_after_retry, 0x0081u);
    vm86_expect_u16("the opcode it should have rewound to",
                    (uint16_t)(probe.ip_after_retry - 1u), 0x0080u);
}

/*
 * *** WAS A KNOWN GAP, FIXED ON MAIN@3a540d8 ***
 *
 * In the first audit this case asserted that nothing wrote insn_ip at
 * all: `vm86_step` cleared its prefixes and dispatched without recording
 * where the instruction began, so the trap's own store was the field's
 * only writer and the value meant "the last host trap" rather than "the
 * instruction being executed". A REP wired to it would have rewound into
 * the middle of the stub.
 *
 * The dispatcher records it now, before the prefix loop. The assertion
 * below is the one that would have caught the omission, and it uses a
 * prefixed instruction so that "before the prefixes" is a different
 * address from "at the opcode" -- the property the comment in step.c
 * claims and which no unprefixed case can tell apart.
 */
static void test_the_dispatcher_records_the_instruction_start(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();

    /* `cs: mov ax, 1`, so the prefix byte and the opcode are different
     * addresses. */
    static const uint8_t code[] = { 0x2E, 0xB8, 0x01, 0x00, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* Something the trap would have left behind, to prove this is a
     * record and not a leftover. */
    cpu->insn_ip = 0xDEADu;

    vm86_step(cpu);

    vm86_expect_u16("the start is the prefix byte, not the opcode",
                    cpu->insn_ip, 0x0100u);
    vm86_expect_u16("and the opcode really is one byte further on",
                    (uint16_t)(cpu->insn_ip + 1u), 0x0101u);

    /* And it follows the machine rather than sticking at the first
     * instruction. */
    vm86_step(cpu);
    vm86_expect_u16("the next instruction has its own start",
                    cpu->insn_ip, 0x0104u);
}

/* ------------------------------------------------------------------ */
/* The retry                                                           */
/* ------------------------------------------------------------------ */

/*
 * What a retrying service does to the machine, end to end.
 *
 * Three claims are checked at once and they are one another's context:
 *
 *   - the service is entered once per attempt, and the attempt after the
 *     last one is the one that returns;
 *   - the stack is where it started when the call finally returns, so a
 *     retry does not leak the frame the INT pushed;
 *   - the guest's IF is what it was before the call, even though the
 *     retry turned interrupts on inside the handler. That is the whole
 *     argument for the retry's `sti` being allowed to override a guest
 *     that called with them off: the override is inside the call.
 *
 * And the machine's own IF is *on* while the service is retrying --
 * without it the run loop would never deliver the interrupt the retry is
 * waiting for. That is checked from inside the service.
 */
struct retry_state {
    unsigned calls;
    unsigned retries_wanted;
    uint16_t sp_first;
    uint16_t sp_last;
    bool     if_after_retry;
};

static void retrying_service(struct vm86_cpu *cpu, void *ctx)
{
    struct retry_state *rs = ctx;

    rs->calls++;

    if (rs->calls == 1)
        rs->sp_first = cpu->sp;

    rs->sp_last = cpu->sp;

    if (rs->calls <= rs->retries_wanted) {
        vm86_service_retry(cpu);
        rs->if_after_retry = (cpu->flags & VM86_IF) != 0;
    }
}

static void test_a_retry_keeps_the_stack_and_gives_the_guest_its_flags_back(
        struct vm86_cpu *cpu)
{
    struct retry_state rs = { 0, 3, 0, 0, false };

    vm86_clear_services();
    vm86_install_ivt(cpu);
    vm86_register_service(0x16, retrying_service, &rs);

    static const uint8_t code[] = { 0xCD, 0x16, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    /* Interrupts off, the way a guest that is about to wait for
     * something would leave them. */
    cpu->flags = VM86_FLAG_ALWAYS_SET;

    enum vm86_result result = vm86_test_run(cpu, 200);

    vm86_expect_u16("the service ran once per attempt", rs.calls, 4);
    vm86_expect_bool("interrupts were on while it retried",
                     rs.if_after_retry, true);
    vm86_expect_u16("the service saw the same stack every attempt",
                    rs.sp_last, rs.sp_first);
    vm86_expect_bool("the machine reached the halt", result == VM86_HALT, true);
    vm86_expect_u16("and the stack came back to where it started",
                    cpu->sp, VM86_TEST_STACK_TOP);
    vm86_expect_bool("and the guest's IF is still off",
                     (cpu->flags & VM86_IF) != 0, false);
}

/*
 * The rewind target is the stub, not the guest's INT.
 *
 * This is the one thing about the retry that host.h describes
 * differently from the way it works: the header says "the frame the INT
 * pushed is popped, and the instruction pointer is rewound to the
 * instruction itself". Nothing pops the frame and the instruction
 * pointed at is the stub's trap.
 *
 * The outcome is the same -- one frame, interrupts live, the guest's
 * flags restored by the stub's IRET -- which is why the difference is
 * worth a case rather than an alarm: a reader who trusts the header will
 * go looking for a pop that is not there, and the next person to write a
 * service that retries will assume a frame was unwound.
 */
static void test_a_retry_rewinds_to_the_stub_and_not_to_the_int(
        struct vm86_cpu *cpu)
{
    struct retry_state rs = { 0, 1, 0, 0, false };

    vm86_clear_services();
    vm86_install_ivt(cpu);
    vm86_register_service(0x16, retrying_service, &rs);

    static const uint8_t code[] = { 0xCD, 0x16, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_step(cpu);                     /* the INT */
    vm86_step(cpu);                     /* the stub, the trap, the retry */

    vm86_expect_u16("the service ran", rs.calls, 1);
    vm86_expect_u16("and it rewound into the stub, not to the INT",
                    cpu->ip, (uint16_t)(0x16u * VM86_TRAP_STRIDE));
    vm86_expect_u16("with the guest's return address still on the stack",
                    vm86_mem_read16(cpu->mem, cpu->sp), 0x0102u);
}

/* ------------------------------------------------------------------ */
/* vm86_interrupt, and whether it is the only frame there is           */
/* ------------------------------------------------------------------ */

/*
 * INT pushes the frame by one implementation and vm86_interrupt() is
 * supposed to be it, for all three paths that deliver an interrupt.
 *
 * Today it is not: the INT instruction goes through do_interrupt() in
 * ops_ctl.c, which is a second copy of the same six pushes and the same
 * two flag writes, and vm86_interrupt() has no caller in the tree at
 * all. This case does not assert that they agree by construction -- it
 * runs both and compares the stacks, so that the day they stop agreeing
 * is the day this fails rather than the day a program returns to the
 * wrong place.
 */
static void test_the_int_instruction_and_vm86_interrupt_agree(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    static const uint8_t code[] = { 0xCD, 0x21, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF | VM86_CF;

    vm86_step(cpu);                     /* the INT instruction */

    uint16_t int_sp   = cpu->sp;
    uint16_t int_ip   = cpu->ip;
    uint16_t int_cs   = cpu->cs;
    uint16_t int_flags = cpu->flags;
    uint16_t frame_return = vm86_mem_read16(cpu->mem, int_sp);
    uint16_t frame_cs     = vm86_mem_read16(cpu->mem, int_sp + 2u);
    uint16_t frame_flags  = vm86_mem_read16(cpu->mem, int_sp + 4u);

    /* The same starting state, delivered by the function instead. The IP
     * has to be moved on by hand, because the INT instruction is what
     * would have moved it. */
    vm86_test_load(cpu, code, sizeof(code));
    cpu->ip    = 0x0102u;
    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF | VM86_CF;

    vm86_interrupt(cpu, 0x21);

    vm86_expect_u16("the same stack pointer", cpu->sp, int_sp);
    vm86_expect_u16("the same vector", cpu->ip, int_ip);
    vm86_expect_u16("the same segment", cpu->cs, int_cs);
    vm86_expect_u16("the same flags afterwards", cpu->flags, int_flags);
    vm86_expect_u16("the same return address on the stack",
                    vm86_mem_read16(cpu->mem, cpu->sp), frame_return);
    vm86_expect_u16("the same saved CS", vm86_mem_read16(cpu->mem, cpu->sp + 2u),
                    frame_cs);
    vm86_expect_u16("the same saved flags",
                    vm86_mem_read16(cpu->mem, cpu->sp + 4u), frame_flags);
}

static void test_interrupt_pushes_flags_cs_ip_in_that_order(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    cpu->sp    = 0xFFF0u;
    cpu->ip    = 0x0100u;
    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF | VM86_CF;

    vm86_interrupt(cpu, 0x10);

    /* Three pushes from FFF0 leave SP at FFEA, and the frame reads from
     * there upward: IP at SP, then CS, then FLAGS furthest away. Popping
     * in that order is what an IRET does. */
    vm86_expect_u16("the stack pointer after three pushes", cpu->sp, 0xFFEAu);
    vm86_expect_mem16("the return offset nearest SP", cpu, 0xFFEAu, 0x0100u);
    vm86_expect_mem16("the return segment", cpu, 0xFFECu, 0x0000u);
    vm86_expect_mem16("the caller's flags furthest away", cpu, 0xFFEEu,
                      (uint16_t)(VM86_FLAG_ALWAYS_SET | VM86_IF | VM86_CF));

    vm86_expect_bool("IF cleared in the processor", (cpu->flags & VM86_IF) != 0,
                     false);
    vm86_expect_bool("TF cleared in the processor", (cpu->flags & VM86_TF) != 0,
                     false);
    vm86_expect_bool("and the frame kept the caller's IF",
                     (vm86_mem_read16(cpu->mem, 0xFFEEu) & VM86_IF) != 0, true);
}

/*
 * vm86_interrupt() leaves cpu->halted alone, and that is a deliberate
 * decision written down in deliver.c: the run loop is where a halted
 * processor is woken, and clearing it here would also clear it for the
 * timer handler chaining to INT 1Ch, where nothing is being woken.
 *
 * The case pins the decision. What it cannot do is check the other half
 * of the contract -- that the run loop does clear halted when it delivers
 * to a halted machine -- because the run loop is not in the tree. That
 * half is carried in the report.
 */
static void test_interrupt_does_not_wake_a_halted_processor(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    cpu->halted = true;

    vm86_interrupt(cpu, 0x08);

    vm86_expect_bool("still halted after an interrupt was delivered",
                     cpu->halted, true);
}

/* ------------------------------------------------------------------ */
/* Pending interrupts                                                  */
/* ------------------------------------------------------------------ */

static void test_raise_sets_one_bit_and_the_lowest_one_wins(struct vm86_cpu *cpu)
{
    vm86_clear_services();

    vm86_expect_u16("nothing pending to start with", (uint16_t)vm86_next_pending(cpu), 0xFFFFu);

    vm86_raise(cpu, 0x1Fu);
    vm86_expect_u16("one vector pending", vm86_next_pending(cpu), 0x1Fu);

    vm86_expect_u16("bit 7 of byte 3", cpu->intr_pending[3], 0x80u);

    vm86_raise(cpu, 0x08u);
    vm86_expect_u16("the lower vector is answered first",
                    vm86_next_pending(cpu), 0x08u);
    vm86_expect_u16("bit 0 of byte 1", cpu->intr_pending[1], 0x01u);

    vm86_clear_pending(cpu, 0x08u);
    vm86_expect_u16("and then the other one", vm86_next_pending(cpu), 0x1Fu);
    vm86_expect_u16("byte 1 is empty again", cpu->intr_pending[1], 0x00u);

    vm86_clear_pending(cpu, 0x1Fu);
    vm86_expect_u16("and now nothing", (uint16_t)vm86_next_pending(cpu), 0xFFFFu);
}

static void test_raising_twice_is_one_interrupt(struct vm86_cpu *cpu)
{
    vm86_clear_services();

    vm86_raise(cpu, 0x09u);
    vm86_raise(cpu, 0x09u);

    vm86_expect_u16("byte 1 holds one bit", cpu->intr_pending[1], 0x02u);

    vm86_clear_pending(cpu, 0x09u);
    vm86_expect_u16("one clear is enough", (uint16_t)vm86_next_pending(cpu), 0xFFFFu);
}

/*
 * Two vectors that share a byte of the map.
 *
 * Worth its own case because a map written with `=` rather than `|=`
 * passes every single-vector case in this file: assigning 1<<bit to a
 * byte that holds nothing else is the same as OR-ing it. The difference
 * only shows when a second vector lands in a byte that already has one,
 * and the symptom on the machine is a peripheral whose interrupt stops
 * being delivered whenever two are pending at once.
 */
static void test_two_vectors_sharing_a_byte_are_both_remembered(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();

    vm86_raise(cpu, 0x08u);
    vm86_raise(cpu, 0x09u);
    vm86_raise(cpu, 0x07u);

    vm86_expect_u16("both bits in byte 1", cpu->intr_pending[1], 0x03u);
    vm86_expect_u16("and the one in byte 0", cpu->intr_pending[0], 0x80u);

    vm86_clear_pending(cpu, 0x08u);
    vm86_expect_u16("clearing one leaves the other", cpu->intr_pending[1], 0x02u);

    vm86_clear_pending(cpu, 0x09u);
    vm86_clear_pending(cpu, 0x07u);
    vm86_expect_u16("and now nothing is pending",
                    (uint16_t)vm86_next_pending(cpu), 0xFFFFu);
}

static void test_interruptible_needs_all_three_conditions(struct vm86_cpu *cpu)
{
    vm86_clear_services();

    cpu->flags      = VM86_FLAG_ALWAYS_SET;     /* IF clear */
    cpu->intr_shadow = 0;

    vm86_raise(cpu, 0x08u);

    vm86_expect_bool("pending but IF is clear", vm86_interruptible(cpu), false);

    vm86_flag_set(cpu, VM86_IF, true);
    vm86_expect_bool("pending and IF set", vm86_interruptible(cpu), true);

    cpu->intr_shadow = 1;
    vm86_expect_bool("but not in the shadow of STI or a load of SS",
                     vm86_interruptible(cpu), false);

    cpu->intr_shadow = 0;
    vm86_clear_pending(cpu, 0x08u);
    vm86_expect_bool("and not with nothing pending",
                     vm86_interruptible(cpu), false);
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

/*
 * A guest handler, at 0x0500, pointed at from a vector.
 *
 * 0x0500 rather than somewhere nearer the program, because in this
 * harness everything below 0x0400 is the vector table and a handler
 * parked inside it would be overwritten by the next one -- see the case
 * on that at the end of this section.
 *
 * Two handlers, because two different things are worth observing. One
 * records that it ran. The other records *where it was entered from*,
 * which is how the grace period after STI is measured: an interrupt
 * delivered one instruction too early says so in that number.
 */
static const uint8_t HANDLER_MARKS[] = {
    0xC6, 0x06, 0x00, 0x06, 0x5A,   /* mov byte [0x0600], 0x5A */
    0xCF,                           /* iret */
};

static const uint8_t HANDLER_RECORDS_IP[] = {
    0x89, 0xE5,                     /* mov bp, sp        */
    0x8B, 0x46, 0x00,               /* mov ax, [bp+0]    */
    0xA3, 0x00, 0x06,               /* mov [0x0600], ax  */
    0xCF,                           /* iret              */
};

#define HANDLER_BASE 0x0500u
#define RECORD_AT    0x0600u

static void hook(struct vm86_cpu *cpu, uint8_t vector,
                 const uint8_t *code, size_t size)
{
    for (size_t i = 0; i < size; i++)
        vm86_mem_write8(cpu->mem, HANDLER_BASE + (uint32_t)i, code[i]);

    vm86_mem_write16(cpu->mem, (uint32_t)vector * 4u,     HANDLER_BASE);
    vm86_mem_write16(cpu->mem, (uint32_t)vector * 4u + 2u, 0x0000u);
}

/*
 * The instruction after STI runs before any interrupt is delivered.
 *
 * That is the whole of what the shadow is for, and it is the one piece
 * of the run loop whose failure is invisible in a program that does not
 * depend on it: an interrupt delivered one instruction early runs a
 * handler that then returns, and the program carries on with the same
 * registers it would have had. The handler records the IP it was entered
 * from, so "one instruction early" is a number and not a shrug.
 *
 * The program is `sti ; nop ; nop ; hlt`, at 0x0100. Everything after
 * the STI is at 0x0101, so an interrupt taken after the grace expired
 * records 0x0102 -- the address of the second nop -- and one taken a
 * boundary too early records 0x0101.
 */
static void test_the_instruction_after_sti_is_not_interrupted(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    hook(cpu, 0x08, HANDLER_RECORDS_IP, sizeof(HANDLER_RECORDS_IP));

    static const uint8_t code[] = { 0xFB, 0x90, 0x90, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_raise(cpu, 0x08);

    vm86_run(cpu, 20);

    vm86_expect_mem16("the interrupt was taken after the instruction "
                      "following STI, not before it", cpu, RECORD_AT, 0x0102u);
}

/*
 * And the grace survives a slice that ends while it is armed.
 *
 * This is the case for the one place run.c departs from the obvious
 * order: the shadow is decremented immediately before the instruction,
 * after the budget check, rather than at the top with the delivery
 * check. Decrementing it at the top (or before the budget check) means a
 * slice that ends between the two eats the grace without running
 * anything, and the *next* call delivers an interrupt inside the window
 * the grace exists to protect.
 *
 * That failure needs a slice to end exactly on the STI, which is why the
 * budget here is one instruction. The recorded IP is the same number as
 * in the case above, and the two differ only in where the slice ended --
 * which is the point.
 */
static void test_the_grace_survives_a_slice_that_ends_in_it(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    hook(cpu, 0x08, HANDLER_RECORDS_IP, sizeof(HANDLER_RECORDS_IP));

    static const uint8_t code[] = { 0xFB, 0x90, 0x90, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_raise(cpu, 0x08);

    enum vm86_stop first = vm86_run(cpu, 1);   /* exactly the STI */

    vm86_expect_bool("the slice ended on the STI", first == VM86_STOP_STEPS,
                     true);
    vm86_expect_u16("and the grace is still armed", cpu->intr_shadow, 1);

    vm86_run(cpu, 20);

    vm86_expect_mem16("the next slice did not interrupt inside the window "
                      "either", cpu, RECORD_AT, 0x0102u);
    vm86_expect_u16("and the grace was spent by the instruction it "
                    "belongs to", cpu->intr_shadow, 0);
}

/*
 * A budget of zero still delivers.
 *
 * `steps` of zero is documented as "is anything waiting that should run,
 * without executing anything", and the order of the loop is what makes
 * that true: delivery comes before the budget check, so a caller polling
 * with a zero budget gets the interrupt taken and no instruction
 * retired. What it means in practice is that the machine is vectored
 * into the handler and stopped there.
 */
static void test_a_zero_budget_still_delivers(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    hook(cpu, 0x08, HANDLER_MARKS, sizeof(HANDLER_MARKS));

    static const uint8_t code[] = { 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF;

    vm86_raise(cpu, 0x08);

    enum vm86_stop result = vm86_run(cpu, 0);

    vm86_expect_bool("nothing was executed", result == VM86_STOP_STEPS, true);
    vm86_expect_u16("but the machine is in the handler", cpu->ip, HANDLER_BASE);
    vm86_expect_bool("in the guest's own segment", cpu->cs == 0, true);
    vm86_expect_mem8("and the handler has not run yet", cpu, RECORD_AT, 0x00);
}

/*
 * `cli ; hlt` is the end of the program, and it is the end this milestone
 * decides programs with.
 *
 * A maskable interrupt cannot wake a machine with IF clear, so there is
 * nothing left that could make progress and the loop says so rather than
 * spinning. This is the case the acceptance path depends on: a program
 * that ends by halting looks identical to one that ended by hanging
 * unless the machine reports which happened.
 */
static void test_cli_then_hlt_is_terminal(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    hook(cpu, 0x08, HANDLER_MARKS, sizeof(HANDLER_MARKS));

    static const uint8_t code[] = { 0xFA, 0xF4 };   /* cli ; hlt */

    vm86_test_load(cpu, code, sizeof(code));

    /* Something is waiting, and it does not matter: IF is clear. */
    vm86_raise(cpu, 0x08);

    enum vm86_stop result = vm86_run(cpu, 100);

    vm86_expect_bool("the machine stopped rather than spun",
                     result == VM86_STOP_HALT, true);
    vm86_expect_bool("and says it is halted", cpu->halted, true);
    vm86_expect_mem8("with the interrupt undelivered", cpu, RECORD_AT, 0x00);
}

/*
 * `sti ; hlt` is not the end of anything -- it is a machine waiting, and
 * an interrupt wakes it.
 *
 * This is the other half of the `cli ; hlt` case and the reason the loop
 * checks delivery before it checks the halted flag. Getting the order
 * wrong loses every wake-up, and the program it loses it for is an idle
 * loop, which is to say an operating system rather than a test.
 */
static void test_sti_then_hlt_is_woken(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    hook(cpu, 0x08, HANDLER_MARKS, sizeof(HANDLER_MARKS));

    static const uint8_t code[] = { 0xFB, 0xF4 };   /* sti ; hlt */

    vm86_test_load(cpu, code, sizeof(code));

    vm86_raise(cpu, 0x08);

    enum vm86_stop result = vm86_run(cpu, 4);

    vm86_expect_bool("the machine was not reported as stopped",
                     result == VM86_STOP_STEPS, true);
    vm86_expect_mem8("the handler ran", cpu, RECORD_AT, 0x5A);
    vm86_expect_bool("and the halt was cleared", cpu->halted, false);
}

/*
 * The same program with nothing waiting is a machine that has stopped,
 * which is what makes the wake-up above a property of the interrupt
 * rather than of the HLT.
 */
static void test_a_halted_machine_with_nothing_to_wake_it_stops(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    static const uint8_t code[] = { 0xFB, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_stop result = vm86_run(cpu, 100);

    vm86_expect_bool("the machine stopped", result == VM86_STOP_HALT, true);
}

static void test_the_budget_ends_the_slice(struct vm86_cpu *cpu)
{
    vm86_clear_services();

    static const uint8_t code[] = { 0x90, 0x90, 0x90, 0x90, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_stop result = vm86_run(cpu, 2);

    vm86_expect_bool("the slice ended", result == VM86_STOP_STEPS, true);
    vm86_expect_u16("after exactly two instructions", cpu->ip, 0x0102u);
}

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

/* `div bl` with BL = 0, which is the machine's own divide error. */
static const uint8_t DIVIDE_BY_ZERO[] = { 0xF6, 0xF3, 0xF4 };

/*
 * An exception on a vector the guest never hooked stops the machine.
 *
 * There is nobody to run, so spinning would be a machine that looks busy
 * and does nothing; stopping and leaving the vector in cpu->fault is
 * what M3 did with every fault and what the existing divide-error cases
 * expect.
 */
static void test_an_unhooked_fault_stops_the_machine(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    vm86_test_load(cpu, DIVIDE_BY_ZERO, sizeof(DIVIDE_BY_ZERO));
    cpu->bl = 0;

    enum vm86_stop result = vm86_run(cpu, 20);

    vm86_expect_bool("the machine stopped", result == VM86_STOP_FAULT, true);
    vm86_expect_u16("with the vector left for the caller", cpu->fault, 0x00u);
}

/*
 * And on a vector the guest did hook, it is delivered like any other
 * interrupt -- which is the rule that tells the machine's own exceptions
 * from the ones a program has taken over, without a table of either.
 */
static void test_a_hooked_fault_is_delivered_to_the_guest(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    hook(cpu, 0x00, HANDLER_MARKS, sizeof(HANDLER_MARKS));

    vm86_test_load(cpu, DIVIDE_BY_ZERO, sizeof(DIVIDE_BY_ZERO));
    cpu->bl = 0;

    enum vm86_stop result = vm86_run(cpu, 20);

    vm86_expect_mem8("the guest's handler ran", cpu, RECORD_AT, 0x5A);
    vm86_expect_bool("and the machine carried on to the halt",
                     result == VM86_STOP_HALT, true);
}

/*
 * An exception is not maskable, so IF has nothing to do with it.
 *
 * The instruction is `cli ; div bl`, with the vector hooked: the fault
 * still reaches the handler. Gating exceptions on IF would make a
 * program that disabled interrupts unable to be told it had divided by
 * zero, and it would be told so by running whatever came next instead.
 */
static void test_an_exception_is_delivered_with_interrupts_off(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    hook(cpu, 0x00, HANDLER_MARKS, sizeof(HANDLER_MARKS));

    static const uint8_t code[] = { 0xFA, 0xF6, 0xF3, 0xF4 };  /* cli; div bl; hlt */

    vm86_test_load(cpu, code, sizeof(code));
    cpu->bl = 0;

    vm86_run(cpu, 20);

    vm86_expect_mem8("the handler ran with interrupts disabled",
                     cpu, RECORD_AT, 0x5A);
}

/* ------------------------------------------------------------------ */
/* Flags coming back out of a service                                  */
/* ------------------------------------------------------------------ */

static void service_sets_carry(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;

    vm86_flag_set(cpu, VM86_CF, true);
}

static void service_clears_interrupts(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;

    vm86_flag_set(cpu, VM86_IF, false);
}

/*
 * A service can hand a flag back to the guest, and that is new.
 *
 * The stub ends in a real IRET, which reloads FLAGS off the frame -- so
 * before this, a service that set carry had it silently overwritten on
 * the way out, and INT 13h's entire error protocol (carry set, code in
 * AH) could not be implemented. Every service's own suite calls the
 * service directly and would not have noticed; it took a guest that
 * branched on the carry.
 */
static void test_a_service_can_hand_a_flag_to_the_guest(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    vm86_register_service(0x21, service_sets_carry, NULL);

    static const uint8_t code[] = { 0xCD, 0x21, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_run(cpu, 20);

    vm86_expect_bool("the carry the service set reached the guest",
                     (cpu->flags & VM86_CF) != 0, true);
}

/*
 * And it still cannot use that channel to change IF or TF.
 *
 * Those two come from the frame rather than from the register, which is
 * what makes the mechanism a return value instead of a bypass: a service
 * cannot leave interrupts disabled on the way out, and it cannot arm the
 * single-step trap on a guest that did not ask for one.
 */
static void test_a_service_cannot_leave_interrupts_off(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);
    vm86_register_service(0x21, service_clears_interrupts, NULL);

    static const uint8_t code[] = { 0xCD, 0x21, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    cpu->flags = VM86_FLAG_ALWAYS_SET | VM86_IF;

    vm86_run(cpu, 20);

    vm86_expect_bool("the guest still has interrupts enabled",
                     (cpu->flags & VM86_IF) != 0, true);
}

/*
 * *** PINS A KNOWN DEFECT ***  -- see M4-intr-audit-report.md
 *
 * A far call pushes two words, not three, so a handler that chains to the
 * previous one with `call far` leaves its own saved CS at SP+2 and
 * whatever was on the stack before that at SP+4. The trap writes a
 * service's flags at SP+4, so it rewrote the second of those: a word the
 * chaining handler was going to use, corrupted on behalf of a frame that
 * was never an interrupt's.
 *
 * This case was written to pin that defect, and its own note said what
 * the expected value would become once it was fixed -- "by checking that
 * the frame is really an interrupt frame, or by having the trap record
 * where it pushed -- the expected value becomes 0xBEEF". The fix took the
 * second of those: vm86_interrupt() records the stack pointer it leaves,
 * and the trap declines to write when the two disagree. The marker
 * therefore survives, and a chained call loses a service's flags instead
 * of gaining a corrupted word.
 *
 * The two paths the firmware installs are unaffected: an INT pushes the
 * three words, and a guest executing the trap bytes for itself has to
 * build the same frame.
 */
static void test_a_chained_far_call_keeps_its_stack(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();

    /* FE 38 10 CF at CS:0x0100, which is the trap calling vector 10h. */
    static const uint8_t stub[] = { 0xFE, 0x38, 0x10, 0xCF };

    for (unsigned i = 0; i < sizeof(stub); i++)
        vm86_mem_write8(cpu->mem, 0x0100u + i, stub[i]);

    cpu->sp    = 0xFFF0u;
    cpu->flags = VM86_FLAG_ALWAYS_SET;

    vm86_mem_write16(cpu->mem, 0xFFF0u, 0x0104u);   /* a far call's IP  */
    vm86_mem_write16(cpu->mem, 0xFFF2u, 0x0000u);   /* a far call's CS  */
    vm86_mem_write16(cpu->mem, 0xFFF4u, 0xBEEF);    /* the caller's own */

    vm86_set_seg(cpu, VM86_CS, 0x0000u);
    cpu->ip = 0x0100u;

    vm86_step(cpu);

    vm86_expect_mem16("the caller's own word, left alone",
                      cpu, 0xFFF4u, 0xBEEFu);
}

/*
 * The harness's program address is inside the vector table.
 *
 * Not a fault in anything this audit was asked to look at -- it is
 * `VM86_TEST_CODE_BASE` in tests/harness.h -- but it changes what every
 * case in this file, and in test_intr.c, actually tests.
 *
 * A .COM program really loads at CS:0x100 with CS set to its own
 * segment, which is never zero; the harness sets CS to zero, so the
 * program's bytes land at linear 0x0100, which in a real machine is
 * inside the interrupt vector table. Loading a program there overwrites
 * the vectors from 0x40 upward, and the only reason the cases above work
 * is that they use vectors below 0x40 and install the table before
 * loading.
 *
 * The consequence for M5 is not small: a program that hooks a vector at
 * 0x60 or above -- the conventional place for a TSR's private API --
 * would have its own code overwrite the vector it just set, and the
 * machine would jump into the middle of the program.
 */
static void test_the_harness_loads_the_program_inside_the_vector_table(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_ivt(cpu);

    vm86_expect_bool("vector 40h is a stub to begin with",
                     vm86_vector_is_stub(cpu, 0x40), true);

    /* Six bytes of program, the size of the smallest real one. */
    static const uint8_t code[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    vm86_expect_bool("and loading the program at 0x0100 unhooked it",
                     vm86_vector_is_stub(cpu, 0x40), false);
    vm86_expect_u16("because the program's first bytes are the vector",
                    vm86_mem_read16(cpu->mem, 0x40u * 4u), 0x9090u);
}

/* ------------------------------------------------------------------ */
/* What the machine says about itself                                  */
/* ------------------------------------------------------------------ */

/*
 * *** WIDENED ON MAIN@3a540d8 ***
 *
 * The first audit found that this install wrote only two words of the
 * data area, which left the video fields at zero -- a program that read
 * the page stride out of 0040:004C before calling INT 10h divided by
 * zero. The install now seeds the display as POST would, so the region
 * this case allows has grown by exactly the bytes POST writes.
 *
 * The scan is unchanged and is the point of the case: memory is filled
 * with a byte no stub or vector can produce, so this answers "these
 * bytes and no others were written" rather than "the region looks right".
 */
static void test_install_firmware_writes_the_table_and_the_machine_description(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();

    memset(cpu->mem->ram, 0xA5, cpu->mem->size);

    vm86_install_firmware(cpu);

    uint32_t bda = (uint32_t)VM86_BDA_SEGMENT << 4;

    vm86_expect_mem16("the equipment word", cpu, bda + VM86_BDA_EQUIPMENT,
                      0x002Du);
    vm86_expect_mem16("the conventional memory size", cpu,
                      bda + VM86_BDA_MEMORY_KB, 640u);

    unsigned stray = 0;
    unsigned in_display = 0;
    unsigned in_extra_display = 0;
    for (uint32_t a = 0; a < cpu->mem->size; a++) {
        if (cpu->mem->ram[a] == 0xA5)
            continue;

        bool in_ivt   = a < 0x400u;
        bool in_stubs = a >= VM86_TRAP_LINEAR
                     && a <  VM86_TRAP_LINEAR + VM86_TRAP_BYTES;

        /* 0040:0010 and 0040:0013 -- four bytes with a gap at 0012 -- and
         * the display half of the data area, 0040:0049 to 0040:0062. */
        bool in_the_two_words = a >= bda + 0x10u && a <= bda + 0x14u;
        bool in_the_display   = a >= bda + 0x49u && a <= bda + 0x62u;

        /* *** WIDENED AGAIN ON MAIN@fcc1d40 ***
         *
         * The video module's audit found that the row count at 0040:0084
         * and the control byte at 0040:0087 had no home in firmware.h, so
         * the service defined them locally -- the same shape as the
         * keyboard service inventing a meaning for 0040:0071 because no
         * map mentioned it. Both are now in the map and both are seeded,
         * because a machine that leaves the row count at zero is a
         * machine with one row and the window before the first INT 10h is
         * exactly where a program reads it.
         *
         * They are allowed as their own two addresses rather than as the
         * range they sit in: "these bytes and no others" is the question,
         * and a range would also permit 0085 and 0086. */
        bool is_extra_display = a == bda + VM86_BDA_ROWS
                             || a == bda + VM86_BDA_VIDEO_CONTROL;

        if (in_the_display)
            in_display++;

        if (is_extra_display)
            in_extra_display++;

        if (!in_ivt && !in_stubs && !in_the_two_words && !in_the_display
            && !is_extra_display)
            stray++;
    }

    vm86_expect_u16("bytes written outside the table and the data area",
                    stray, 0);

    /*
     * Twenty-four, and every one of them is a field a program can read:
     * the mode, the column count, the page stride, the active page, the
     * cursor shape, and eight cursor positions.
     */
    vm86_expect_u16("bytes written into the display half of the data area",
                    in_display, 24u);

    vm86_expect_u16("and the two fields the map was missing",
                    in_extra_display, 2u);
}

/*
 * The equipment word, bit by bit, against the table in docs/dos-refs.md
 * section 2.
 *
 * *** CHANGED ON MAIN@3a540d8 ***
 *
 * The first audit reported that bits 2-3 read 00 -- "16K of motherboard
 * RAM" -- while the same install wrote 640 into 0040:0013 two bytes
 * later, and that INT 11h is exactly the channel that hands the
 * contradiction to a program. They now read 11.
 *
 * The assertion that matters is the arithmetic, not the constant: `11`
 * in bits 2-3 is 0x0C, and the word is 0x21 | 0x0C = 0x2D. Writing 0x31
 * -- which is what "set bits 2-3" looks like if you reach for a number
 * that has 3 in it -- moves bits 4 and 5 as well and turns the display
 * monochrome. The audit made that mistake once; the case is written so
 * the next person does not.
 */
static void test_the_equipment_word_decoded(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_firmware(cpu);

    uint32_t bda = (uint32_t)VM86_BDA_SEGMENT << 4;
    uint16_t word = vm86_mem_read16(cpu->mem, bda + VM86_BDA_EQUIPMENT);

    vm86_expect_u16("the whole word", word, 0x002Du);
    vm86_expect_u16("bit 0: a diskette drive is attached", word & 0x0001u, 1u);
    vm86_expect_u16("bit 1: no 8087", word & 0x0002u, 0u);
    vm86_expect_u16("bits 2-3: motherboard RAM reads as 64K or more",
                    (word >> 2) & 3u, 3u);
    vm86_expect_u16("bits 4-5: initial video is 80x25 colour",
                    (word >> 4) & 3u, 2u);
    vm86_expect_u16("bits 6-7: one diskette drive", (word >> 6) & 3u, 0u);
    vm86_expect_u16("bit 8: no DMA", word & 0x0100u, 0u);
    vm86_expect_u16("bits 9-11: no serial ports", (word >> 9) & 7u, 0u);
    vm86_expect_u16("bit 12: no game port", word & 0x1000u, 0u);
    vm86_expect_u16("bit 13: no serial printer", word & 0x2000u, 0u);
    vm86_expect_u16("bits 14-15: no parallel ports", (word >> 14) & 3u, 0u);

    /* And the word agrees with the machine it describes. */
    vm86_expect_u16("a 640K machine does not claim a 16K motherboard",
                    (word >> 2) & 3u, 3u);
}

/*
 * Both services answer out of the data area rather than out of a
 * constant, which is the property that makes the two ways of asking --
 * the call and the memory -- the same answer. Changing the memory and
 * calling again is what tells the two apart.
 */
static void test_int_11h_and_12h_answer_from_the_data_area(struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_firmware(cpu);

    static const uint8_t ask_11[] = { 0xB8, 0x00, 0x00, 0xCD, 0x11, 0xF4 };
    static const uint8_t ask_12[] = { 0xB8, 0x00, 0x00, 0xCD, 0x12, 0xF4 };

    vm86_test_load(cpu, ask_11, sizeof(ask_11));
    vm86_test_run(cpu, 100);
    vm86_expect_u16("INT 11h answers the equipment word", cpu->ax, 0x002Du);

    vm86_test_load(cpu, ask_12, sizeof(ask_12));
    vm86_test_run(cpu, 100);
    vm86_expect_u16("INT 12h answers the memory size", cpu->ax, 640u);

    /* Rewrite what the data area says. A service that answered from its
     * own copy would not notice. */
    uint32_t bda = (uint32_t)VM86_BDA_SEGMENT << 4;
    vm86_mem_write16(cpu->mem, bda + VM86_BDA_EQUIPMENT, 0x1234u);
    vm86_mem_write16(cpu->mem, bda + VM86_BDA_MEMORY_KB, 512u);

    vm86_test_load(cpu, ask_11, sizeof(ask_11));
    vm86_test_run(cpu, 100);
    vm86_expect_u16("INT 11h follows the data area", cpu->ax, 0x1234u);

    vm86_test_load(cpu, ask_12, sizeof(ask_12));
    vm86_test_run(cpu, 100);
    vm86_expect_u16("and so does INT 12h", cpu->ax, 512u);
}

/*
 * *** WAS A KNOWN GAP, FIXED ON MAIN@3a540d8 ***
 *
 * The first audit found that this install left every video word at zero:
 * a program that read 0040:004C for the page stride before calling
 * INT 10h divided by zero, and one that read 0040:0049 thought the
 * machine was in mode 0. That is the documented division of labour --
 * the fields belong to the video service -- and it was also a window
 * nothing covered.
 *
 * The install now writes what POST would. The assertions are not "these
 * constants" but "this is a machine that describes itself coherently",
 * which is the property the fix is for: a mode, a width and a stride
 * that agree with each other and with a page of eighty by twenty-five
 * cells.
 */
static void test_install_firmware_seeds_a_coherent_display(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_firmware(cpu);

    uint32_t bda = (uint32_t)VM86_BDA_SEGMENT << 4;

    uint8_t  mode    = vm86_mem_read8 (cpu->mem, bda + VM86_BDA_VIDEO_MODE);
    uint16_t columns = vm86_mem_read16(cpu->mem, bda + VM86_BDA_COLUMNS);
    uint16_t stride  = vm86_mem_read16(cpu->mem, bda + VM86_BDA_PAGE_BYTES);

    vm86_expect_u16("the mode is a text mode this machine has", mode, 0x03u);
    vm86_expect_u16("eighty columns", columns, 80u);
    vm86_expect_u16("a stride that is not zero", stride, 0x1000u);

    /* The stride has to hold a page. It is not 80*25*2 = 4000 -- it is
     * rounded up to 4096 -- so the check is "at least", not "equal". */
    vm86_expect_bool("and the stride holds a whole page of that size",
                     stride >= (uint16_t)(columns * 25u * 2u), true);

    vm86_expect_mem8("page 0 is the page being displayed", cpu,
                     bda + VM86_BDA_ACTIVE_PAGE, 0);
    vm86_expect_mem16("with a cursor shape that draws", cpu,
                      bda + VM86_BDA_CURSOR_SHAPE, 0x0607u);
    vm86_expect_mem16("and the cursor at home", cpu,
                      bda + VM86_BDA_CURSOR, 0);

    for (uint16_t page = 1; page < 8; page++)
        vm86_expect_mem16("every page's cursor starts at home", cpu,
                          bda + VM86_BDA_CURSOR + page * 2u, 0);
}

/*
 * The row count and the video control byte, which the map gained last.
 *
 * Both are stored as the odd thing rather than the obvious one, and both
 * are the kind of field where the obvious value is wrong in a way nothing
 * notices: the row count is one less than the number of rows, so a
 * machine that writes 25 there claims twenty-six rows and a machine that
 * leaves it at zero claims one -- and the control byte's bit 7 is inverted
 * in sense from its name, since *clear* means modes do clear memory.
 */
static void test_install_firmware_seeds_the_last_row_and_the_control_byte(
        struct vm86_cpu *cpu)
{
    vm86_clear_services();
    vm86_install_firmware(cpu);

    uint32_t bda = (uint32_t)VM86_BDA_SEGMENT << 4;

    vm86_expect_mem8("0040:0084 is rows minus one, not rows",
                     cpu, bda + VM86_BDA_ROWS, 24u);
    vm86_expect_mem8("0040:0087 has bit 7 clear, which means mode sets "
                     "do clear memory", cpu, bda + VM86_BDA_VIDEO_CONTROL, 0u);
}

/*
 * And the Ctrl-Break flag is seeded by nobody at all.
 *
 * It is the one field of the three that this machine deliberately does
 * not maintain -- Ctrl-Break is not decoded -- and the failure it is
 * guarding against is the opposite of the others': not a zero where a
 * value belongs, but a value where nothing belongs. A machine that
 * started up with bit 7 of that byte set would report the user having
 * pressed Break before the keyboard was ever touched, and every program
 * that polls it would take the branch.
 *
 * Nothing in the tree writes it, which is the state to keep; this case
 * fails if anything ever starts.
 */
static void test_nothing_seeds_the_ctrl_break_flag(struct vm86_cpu *cpu)
{
    vm86_clear_services();

    memset(cpu->mem->ram, 0, cpu->mem->size);

    vm86_install_firmware(cpu);

    vm86_expect_mem8("nothing was seeded there", cpu,
                     ((uint32_t)VM86_BDA_SEGMENT << 4) + VM86_BDA_CTRL_BREAK,
                     0u);
}

/* ------------------------------------------------------------------ */
/* The registry                                                        */
/* ------------------------------------------------------------------ */

static void test_registering_null_removes_a_service(struct vm86_cpu *cpu)
{
    struct counter c = { 0, 0 };

    vm86_clear_services();
    vm86_install_ivt(cpu);

    vm86_register_service(0x10, counting_service, &c);
    vm86_register_service(0x10, NULL, NULL);

    static const uint8_t code[] = { 0xCD, 0x10, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));
    vm86_test_run(cpu, 100);

    vm86_expect_u16("the service did not run", c.calls, 0);
}

static void test_clear_services_removes_everything(struct vm86_cpu *cpu)
{
    struct counter c = { 0, 0 };

    vm86_clear_services();
    vm86_install_ivt(cpu);

    /* Every vector that any service file in this tree uses. */
    vm86_register_service(0x10, counting_service, &c);
    vm86_register_service(0x13, counting_service, &c);
    vm86_register_service(0x16, counting_service, &c);
    vm86_register_service(0x1A, counting_service, &c);

    vm86_clear_services();

    static const uint8_t code[] = { 0xCD, 0x10, 0xCD, 0x13, 0xCD, 0x16,
                                    0xCD, 0x1A, 0xF4 };

    vm86_test_load(cpu, code, sizeof(code));

    enum vm86_result result = vm86_test_run(cpu, 100);

    vm86_expect_u16("none of them ran", c.calls, 0);
    vm86_expect_bool("and the program carried on anyway",
                     result == VM86_HALT, true);
}

/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "every vector points at its own stub",
      test_every_vector_points_at_its_own_stub },
    { "every stub is the four bytes it should be",
      test_every_stub_is_four_bytes_of_the_right_shape },
    { "install_ivt writes its own region and no more",
      test_install_ivt_writes_its_own_region_and_no_more },
    { "a hooked vector stops reading as a stub",
      test_a_hooked_vector_stops_reading_as_a_stub },
    { "a guest storing the same vector reads as unhooked",
      test_a_guest_that_stores_the_same_vector_reads_as_unhooked },

    { "INT reaches the service and returns",
      test_int_reaches_the_service_and_returns },
    { "an unregistered vector is harmless",
      test_an_unregistered_vector_is_harmless },
    { "a guest can execute the trap itself",
      test_a_guest_can_execute_the_trap_itself },

    { "the trap records its own start",
      test_the_trap_records_its_own_start },
    { "KNOWN DEFECT: a prefix in front of the trap is not accounted for",
      test_a_prefix_in_front_of_the_trap_is_not_accounted_for },
    { "KNOWN DEFECT: a displacement moves the recorded instruction start",
      test_a_displacement_between_the_trap_and_its_service_byte },
    { "the dispatcher records the instruction start",
      test_the_dispatcher_records_the_instruction_start },

    { "a retry keeps the stack and gives the guest its flags back",
      test_a_retry_keeps_the_stack_and_gives_the_guest_its_flags_back },
    { "a retry rewinds to the stub and not to the INT",
      test_a_retry_rewinds_to_the_stub_and_not_to_the_int },

    { "the INT instruction and vm86_interrupt agree",
      test_the_int_instruction_and_vm86_interrupt_agree },
    { "vm86_interrupt pushes flags, CS and IP in that order",
      test_interrupt_pushes_flags_cs_ip_in_that_order },
    { "vm86_interrupt does not wake a halted processor",
      test_interrupt_does_not_wake_a_halted_processor },

    { "raise sets one bit and the lowest vector wins",
      test_raise_sets_one_bit_and_the_lowest_one_wins },
    { "raising twice is one interrupt",
      test_raising_twice_is_one_interrupt },
    { "two vectors sharing a byte are both remembered",
      test_two_vectors_sharing_a_byte_are_both_remembered },
    { "interruptible needs all three conditions",
      test_interruptible_needs_all_three_conditions },

    { "the instruction after STI is not interrupted",
      test_the_instruction_after_sti_is_not_interrupted },
    { "the grace survives a slice that ends in it",
      test_the_grace_survives_a_slice_that_ends_in_it },
    { "a zero budget still delivers",
      test_a_zero_budget_still_delivers },
    { "cli then hlt is terminal",
      test_cli_then_hlt_is_terminal },
    { "sti then hlt is woken by an interrupt",
      test_sti_then_hlt_is_woken },
    { "a halted machine with nothing to wake it stops",
      test_a_halted_machine_with_nothing_to_wake_it_stops },
    { "the budget ends the slice",
      test_the_budget_ends_the_slice },

    { "an unhooked fault stops the machine",
      test_an_unhooked_fault_stops_the_machine },
    { "a hooked fault is delivered to the guest",
      test_a_hooked_fault_is_delivered_to_the_guest },
    { "an exception is delivered with interrupts off",
      test_an_exception_is_delivered_with_interrupts_off },

    { "a service can hand a flag to the guest",
      test_a_service_can_hand_a_flag_to_the_guest },
    { "a service cannot leave interrupts off",
      test_a_service_cannot_leave_interrupts_off },
    { "a chained far call keeps its stack",
      test_a_chained_far_call_keeps_its_stack },

    { "install_firmware writes the table and the machine description",
      test_install_firmware_writes_the_table_and_the_machine_description },
    { "the equipment word decoded bit by bit",
      test_the_equipment_word_decoded },
    { "INT 11h and INT 12h answer from the data area",
      test_int_11h_and_12h_answer_from_the_data_area },
    { "install_firmware seeds a coherent display",
      test_install_firmware_seeds_a_coherent_display },
    { "install_firmware seeds the last row and the control byte",
      test_install_firmware_seeds_the_last_row_and_the_control_byte },
    { "nothing seeds the Ctrl-Break flag",
      test_nothing_seeds_the_ctrl_break_flag },

    { "registering NULL removes a service",
      test_registering_null_removes_a_service },
    { "clear_services removes everything",
      test_clear_services_removes_everything },

    { "KNOWN HARNESS: the program loads inside the vector table",
      test_the_harness_loads_the_program_inside_the_vector_table },
};

VM86_TEST_MAIN("intr_audit", tests)
