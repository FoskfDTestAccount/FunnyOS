/*
 * Interrupts: the trap, delivery, the shadow, and the run loop.
 *
 * ---------------------------------------------------------------------
 * What this suite is really testing
 *
 * The instruction layer has its own suites and they know nothing about
 * interrupts. Everything here is about the layer above it: the vector
 * table, the stubs in it, the four-byte trap that a guest reaches the
 * host through, and the loop that decides between delivering an interrupt
 * and executing one instruction.
 *
 * That loop is where almost everything here is really aimed. It is the
 * only piece of this milestone that is not a pure function, and it is the
 * only piece whose correctness is an *order* rather than a computation:
 * the shadow, then delivery, then the halt check, then the budget, then
 * one instruction. Get the order wrong and every individual piece still
 * works, which is exactly why each of the four positions has a case below
 * that fails if it moves.
 *
 * ---------------------------------------------------------------------
 * What "the interrupt was delivered" means in an assertion
 *
 * A guest handler is a few bytes of guest code that leaves evidence in
 * guest memory: a count, and whatever the registers held at the moment it
 * ran. A host service leaves evidence in a file-scope variable, because
 * it is C. Both are used, and deliberately: the trap and the run loop
 * have to work with a service on the other end (that is what firmware
 * is), and they have to work with a handler the guest installed (that is
 * what a TSR is, and what the exception rule is about).
 *
 * ---------------------------------------------------------------------
 * Where the guest's things live
 *
 * The vector table is the first kilobyte and the BIOS data area is the
 * hundred bytes after it, so a program cannot be loaded where DOS would
 * put one -- offset 0x100 of a segment -- without the firmware being
 * written over its first instruction. The programs here sit at offset
 * 0x500 instead, well clear of both, and the handlers at 0x680. Nothing
 * depends on the offset; it only has to be somewhere the firmware does
 * not own.
 *
 * ---------------------------------------------------------------------
 * On proving these can fail
 *
 * Every case below was checked by breaking the implementation on purpose
 * and confirming the case goes red. The mutations and which case caught
 * each one are listed in the report; the ones worth naming here are the
 * two that no other suite can reach -- the REP rewind, where rewinding to
 * just past the opcode turns `rep movsb` into `movsb` and copies one byte
 * of a buffer, and the shadow decrement, where spending it at a boundary
 * the budget then cuts at loses the wake-up in an idle loop.
 */
#include "harness.h"

#include <vm86/firmware.h>
#include <vm86/host.h>

/* ------------------------------------------------------------------ */
/* The layout this suite uses                                          */
/* ------------------------------------------------------------------ */

#define CODE    0x0500u   /* where the program is loaded              */
#define HANDLER 0x0680u   /* where a guest interrupt handler goes     */
#define SCRATCH 0x0700u   /* where a handler leaves what it saw       */
#define SOURCE  0x0800u   /* string data                              */
#define DEST    0x0900u   /* where a string instruction writes        */

/* Guest programs end with HLT, and the ones here that expect to be woken
 * have one after it as well -- a machine that stops at the first HLT and
 * a machine that runs on are told apart by whether the bytes after it
 * ran, and by nothing else. */

/* ------------------------------------------------------------------ */
/* Evidence                                                            */
/* ------------------------------------------------------------------ */

#define LOG_MAX 8

static uint8_t  g_log[LOG_MAX];   /* which vectors' services ran, in order */
static size_t   g_log_len;
static uint16_t g_seen_ip;        /* registers as the service found them   */
static uint16_t g_seen_ax;
static uint16_t g_seen_cx;
static uint16_t g_seen_si;

static void forget_evidence(void)
{
    g_log_len = 0;
    g_seen_ip = 0;
    g_seen_ax = 0;
    g_seen_cx = 0;
    g_seen_si = 0;
}

/*
 * A service that records which vector called it and what the registers
 * held. The vector arrives through ctx because a service is not told
 * which one it is -- the registry is indexed by vector and the function
 * is the same function whatever it is registered under.
 */
static void recording_service(struct vm86_cpu *cpu, void *ctx)
{
    if (g_log_len < LOG_MAX)
        g_log[g_log_len++] = (uint8_t)(uintptr_t)ctx;

    g_seen_ip = cpu->ip;
    g_seen_ax = cpu->ax;
    g_seen_cx = cpu->cx;
    g_seen_si = cpu->si;
}

/*
 * A service that raises a hardware interrupt and does nothing else.
 *
 * It stands in for a device, and it exists because of a limit in how a
 * test can reach a machine: the host only gets control between slices, so
 * it can only make an interrupt pending at a slice boundary. Several
 * cases below need one to become pending at a chosen boundary *inside* a
 * program -- right before a MOV SS, or after a string instruction has
 * already moved an element -- and a service called from the program is
 * the only thing that can do that.
 */
static void arming_service(struct vm86_cpu *cpu, void *ctx)
{
    vm86_raise(cpu, (uint8_t)(uintptr_t)ctx);
}

/*
 * The two halves of a blocking read, and the thing they are waiting for.
 *
 * The read refuses until the key is ready, which is what makes the retry
 * mechanism run at all; the keyboard service sets that flag, which is
 * what an interrupt handler does when a keystroke arrives. Neither is
 * allowed to reach into the other -- they only ever meet in the middle,
 * through a hardware interrupt the run loop delivers.
 */
static bool g_key_ready;
static int  g_reads;

static void keyboard_service(struct vm86_cpu *cpu, void *ctx)
{
    (void)cpu;
    (void)ctx;

    g_key_ready = true;
}

static void blocking_read_service(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;

    g_reads++;

    if (!g_key_ready) {
        /*
         * No key. The instruction is re-run rather than answered with
         * whatever happens to be in the accumulator -- a program waiting
         * for a keypress that is handed a phantom one carries on and
         * does something else entirely.
         */
        vm86_service_retry(cpu);
        return;
    }

    cpu->al = 0x41;   /* 'A' */
}

/* ------------------------------------------------------------------ */
/* Building a machine                                                  */
/* ------------------------------------------------------------------ */

static void poke8(struct vm86_cpu *cpu, uint32_t at, uint8_t value)
{
    vm86_mem_write8(cpu->mem, at, value);
}

static void poke16(struct vm86_cpu *cpu, uint32_t at, uint16_t value)
{
    vm86_mem_write16(cpu->mem, at, value);
}

static void place(struct vm86_cpu *cpu, uint32_t at,
                  const uint8_t *code, size_t size)
{
    for (size_t i = 0; i < size; i++)
        poke8(cpu, at + i, code[i]);
}

/*
 * A machine with firmware on it and a program loaded.
 *
 * The order matters twice. The services are cleared first, because the
 * registry is process-wide and a service registered by the previous case
 * would otherwise still be answering -- which reads as a bug in the case
 * that did not register it. And the firmware goes in before the program
 * is written, because the firmware's own writes are what would clobber a
 * program loaded where DOS would have put it.
 *
 * The processor is reset rather than patched up. A case that builds a
 * second machine would otherwise inherit the first one's halted flag and
 * pending vectors and measure those instead of its own program -- and the
 * failure looks like the second program never running, which points at
 * the program rather than at the harness.
 */
static void machine(struct vm86_cpu *cpu, const uint8_t *code, size_t size)
{
    vm86_clear_services();
    forget_evidence();

    vm86_reset(cpu, cpu->mem);
    vm86_install_firmware(cpu);

    place(cpu, CODE, code, size);

    vm86_set_seg(cpu, VM86_CS, 0);
    vm86_set_seg(cpu, VM86_DS, 0);
    vm86_set_seg(cpu, VM86_ES, 0);
    vm86_set_seg(cpu, VM86_SS, 0);
    vm86_flush_segments(cpu);

    cpu->ip = CODE;
    cpu->sp = VM86_TEST_STACK_TOP;
}

/* Point a vector at guest code, the way a program that wants to handle an
 * interrupt does -- and the way a TSR does when it takes one over. */
static void hook(struct vm86_cpu *cpu, uint8_t vector, uint16_t offset)
{
    poke16(cpu, (uint32_t)vector * 4u, offset);
    poke16(cpu, (uint32_t)vector * 4u + 2u, 0x0000);
}

static void interrupt_enable(struct vm86_cpu *cpu)
{
    vm86_flag_set(cpu, VM86_IF, true);
}

/* ------------------------------------------------------------------ */
/* The trap                                                            */
/* ------------------------------------------------------------------ */

/*
 * A call reaches the service and the guest carries on.
 *
 * The whole round trip in one case, because the three parts of it are
 * only correct together: the trap is reached through the stub (so the
 * service runs with the vector the stub names), it leaves the instruction
 * pointer on the stub's IRET (so the next step performs the return rather
 * than the host doing it), and the guest is therefore standing on the
 * instruction after its INT with its stack exactly as it left it.
 *
 * The stack is asserted to the word. A trap that unwound the frame itself
 * would leave SP two or six bytes high here and the guest would return to
 * an address built out of whatever it happened to have there -- which is
 * the failure the stub's own IRET exists to avoid.
 */
static void test_a_trap_call_comes_back(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xCD, 0x80,   /* int 80h   */
        0x40,         /* inc ax    */
        0xF4,         /* hlt       */
    };

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x80, recording_service, (void *)(uintptr_t)0x80);

    uint16_t sp_before = cpu->sp;
    cpu->ax = 0;

    vm86_run(cpu, 20);

    vm86_expect_bool("halted", cpu->halted, true);
    vm86_expect_u16("the service ran once", (uint16_t)g_log_len, 1);
    vm86_expect_u16("on the vector the stub named", g_log[0], 0x80);
    vm86_expect_u16("the guest ran on", cpu->ax, 1);

    /* The trap stops on the byte after the service number, which is the
     * stub's IRET: FE 38 <v> CF, and the trap consumed the first two and
     * the <v>. */
    vm86_expect_u16("IP stopped on the stub's IRET", g_seen_ip,
                    VM86_TRAP_BASE + (0x80u * VM86_TRAP_STRIDE) + 3u);

    vm86_expect_u16("SP is exactly where it started", cpu->sp, sp_before);
}

/*
 * Two services on two vectors are told apart.
 *
 * Worth its own case because the obvious wrong implementation -- "run the
 * service that is registered", with one slot -- passes the round trip
 * above completely. The vector in the stub is the only thing that decides
 * which one runs, and a machine that ignores it answers every call with
 * whichever service it happens to hold.
 */
static void test_the_stub_chooses_the_service(struct vm86_cpu *cpu)
{
    static const uint8_t to_81[] = { 0xCD, 0x81, 0xF4 };   /* int 81h */
    static const uint8_t to_82[] = { 0xCD, 0x82, 0xF4 };   /* int 82h */

    machine(cpu, to_81, sizeof(to_81));
    vm86_register_service(0x81, recording_service, (void *)(uintptr_t)0x81);
    vm86_register_service(0x82, recording_service, (void *)(uintptr_t)0x82);
    vm86_run(cpu, 20);

    vm86_expect_u16("only the one that was called", (uint16_t)g_log_len, 1);
    vm86_expect_u16("which is 81h", g_log[0], 0x81);

    machine(cpu, to_82, sizeof(to_82));
    vm86_register_service(0x81, recording_service, (void *)(uintptr_t)0x81);
    vm86_register_service(0x82, recording_service, (void *)(uintptr_t)0x82);
    vm86_run(cpu, 20);

    vm86_expect_u16("and the other way round", (uint16_t)g_log_len, 1);
    vm86_expect_u16("which is 82h", g_log[0], 0x82);
}

/*
 * A vector nobody registered is not an error.
 *
 * This is firmware behaviour and not an oversight: a real BIOS has no
 * handler for most of its table, and a program that calls one gets the
 * IRET and nothing else. Turning it into an exception would break
 * programs that probe the table, and turning it into a fault would break
 * the ones that call a service this machine does not implement -- of
 * which M4 has several on purpose, INT 14h and INT 17h among them.
 */
static void test_an_unregistered_vector_is_not_an_error(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xCD, 0x33,   /* int 33h -- a mouse driver, and there is none */
        0x40,         /* inc ax   */
        0xF4,         /* hlt      */
    };

    machine(cpu, code, sizeof(code));
    cpu->ax = 0;

    enum vm86_stop stop = vm86_run(cpu, 20);

    vm86_expect_bool("the machine stopped by halting",
                     stop == VM86_STOP_HALT, true);
    vm86_expect_bool("no service ran", g_log_len == 0, true);
    vm86_expect_u16("and the guest carried on", cpu->ax, 1);
}

/* ------------------------------------------------------------------ */
/* What is pending                                                     */
/* ------------------------------------------------------------------ */

/*
 * A vector raised twice is pending once.
 *
 * The 8259 does not queue: a line asserted while its previous interrupt
 * has not been taken yet is one interrupt, and the guest gets one. A
 * machine that counted them would let a device faster than the guest
 * build a backlog that could never be worked off -- and the symptom is
 * not an error but a machine that spends all its time in a handler.
 *
 * The guest here does nothing but halt, so the only thing that can run is
 * the service, and the count of calls is the whole assertion.
 */
static void test_a_vector_raised_twice_is_pending_once(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };   /* hlt */

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);
    interrupt_enable(cpu);

    vm86_raise(cpu, 0x08);
    vm86_raise(cpu, 0x08);

    vm86_run(cpu, 20);

    vm86_expect_u16("delivered once", (uint16_t)g_log_len, 1);
    vm86_expect_u16("on vector 08h", g_log[0], 0x08);
}

/*
 * With two vectors pending, the lower one goes first.
 *
 * Vector numbers are a priority order on this hardware rather than a set
 * of labels: the 8259 presents its lines to the processor in a fixed
 * sequence, and the firmware's handlers are numbered to match. So the
 * machine delivers the lowest pending vector, and a loop that took
 * whichever bit it happened to find would report them in whatever order
 * the bitmap words fall -- which is a clock that runs slow under load
 * rather than an obvious failure.
 *
 * The service for each vector returns through the stub's IRET, which
 * restores the flags the INT pushed, IF among them. That is what lets the
 * second interrupt be delivered in the same run rather than waiting for
 * the guest to enable interrupts again.
 */
static void test_the_lowest_pending_vector_is_delivered_first(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };   /* hlt */

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);
    vm86_register_service(0x09, recording_service, (void *)(uintptr_t)0x09);
    interrupt_enable(cpu);

    /* Raised high first, so that a machine which delivered in the order
     * they arrived would produce the other answer. */
    vm86_raise(cpu, 0x09);
    vm86_raise(cpu, 0x08);

    vm86_run(cpu, 40);

    vm86_expect_u16("two interrupts were delivered", (uint16_t)g_log_len, 2);
    vm86_expect_u16("the lower one first", g_log[0], 0x08);
    vm86_expect_u16("then the other",      g_log[1], 0x09);
}

/* ------------------------------------------------------------------ */
/* The shadow                                                          */
/* ------------------------------------------------------------------ */

/*
 * STI does not let an interrupt in on the instruction that follows it.
 *
 * The discriminator is the instruction in between. `inc ax` runs before
 * the interrupt is delivered, so a service that finds AX holding 1 is on
 * a machine that honoured the delay, and one that finds 0 is on a machine
 * that took the interrupt between the STI and the instruction after it --
 * which on real hardware is the difference between an idle loop that
 * sleeps and one that spins.
 *
 * IF itself is set immediately and this suite does not disturb that;
 * test_verify.c has the case for it. What is delayed is the recognition.
 */
static void test_sti_delays_recognition_by_one_instruction(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xFB,         /* sti    */
        0x40,         /* inc ax */
        0xF4,         /* hlt    */
    };

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);

    cpu->ax = 0;          /* IF is already clear out of reset */
    vm86_raise(cpu, 0x08);

    vm86_run(cpu, 20);

    vm86_expect_bool("delivered at all", g_log_len == 1, true);
    vm86_expect_u16("the instruction after the STI ran first", g_seen_ax, 1);
    vm86_expect_u16("and so did the guest", cpu->ax, 1);
}

/*
 * So does a load of SS, which is the one that keeps a stack switch
 * atomic.
 *
 * `mov ss, ax` / `mov sp, ...` is written without a CLI around it on real
 * hardware, and it is correct as written: an interrupt arriving between
 * the two would run on a stack pointer half of which belongs to the old
 * stack. The window is one instruction, and this is that window.
 *
 * The interrupt is raised by a service rather than by the host between
 * runs, because the boundary where it has to still be pending is the one
 * right after the `mov ss` -- and a host that raised it earlier would
 * have had it delivered before the MOV SS executed. The STI before it
 * costs nothing: it is there so that IF is set for the delivery that the
 * test is waiting for, and its own shadow covers the MOV SS harmlessly.
 */
static void test_mov_ss_delays_recognition_by_one_instruction(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xB8, 0x00, 0x20,   /* mov ax, 2000h   */
        0xCD, 0x80,         /* int 80h         */
        0xFB,               /* sti             */
        0x8E, 0xD0,         /* mov ss, ax      */
        0x40,               /* inc ax          */
        0xF4,               /* hlt             */
    };

    machine(cpu, code, sizeof(code));

    /* The service the INT calls raises the interrupt the test is about. */
    vm86_register_service(0x80, arming_service, (void *)(uintptr_t)0x08);
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);

    cpu->ax = 0;

    vm86_run(cpu, 40);

    vm86_expect_bool("the armed interrupt was delivered", g_log_len == 1, true);
    vm86_expect_u16("on vector 08h", g_log[0], 0x08);
    vm86_expect_u16("the instruction after the MOV SS ran first",
                    g_seen_ax, 0x2001);
    vm86_expect_u16("SS did take its new value", cpu->ss, 0x2000);
}

/*
 * And so does POP SS, which is a different instruction in a different
 * file.
 *
 * The segment pop encodings live in the arithmetic block rather than with
 * the stack instructions, because that is where the 8086's opcode map
 * puts them: 17 is `pop ss`, and it is the same eight-way group as 06 and
 * 0E. An implementation that only knows about `mov ss, ax` -- which is
 * the one every example uses -- leaves this half of the rule undone, and
 * nothing else in the tree would notice.
 */
static void test_pop_ss_delays_recognition_by_one_instruction(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xB8, 0x00, 0x20,   /* mov ax, 2000h   */
        0x50,               /* push ax         */
        0xCD, 0x80,         /* int 80h         */
        0xFB,               /* sti             */
        0x17,               /* pop ss          */
        0x40,               /* inc ax          */
        0xF4,               /* hlt             */
    };

    machine(cpu, code, sizeof(code));

    vm86_register_service(0x80, arming_service, (void *)(uintptr_t)0x08);
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);

    cpu->ax = 0;

    vm86_run(cpu, 40);

    vm86_expect_bool("the armed interrupt was delivered", g_log_len == 1, true);
    vm86_expect_u16("on vector 08h", g_log[0], 0x08);
    vm86_expect_u16("the instruction after the POP SS ran first",
                    g_seen_ax, 0x2001);
    vm86_expect_u16("SS took the popped value", cpu->ss, 0x2000);
}

/* ------------------------------------------------------------------ */
/* HLT                                                                 */
/* ------------------------------------------------------------------ */

/*
 * `sti; hlt` is woken, not stopped.
 *
 * This is the case the whole ordering of the run loop exists for. HLT
 * sets the halted flag and returns VM86_HALT; the loop does not stop
 * there, it comes back round to the top, and it is the delivery step that
 * decides whether the machine was waiting or finished. A loop that
 * returned as soon as HLT executed would report a stopped machine for
 * every idle loop in every program.
 *
 * The instruction after the HLT is the evidence. A machine that was woken
 * returns from the interrupt to it and runs it; a machine that was
 * stopped never gets there.
 */
static void test_sti_hlt_is_woken_rather_than_stopped(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xFB,   /* sti                     */
        0xF4,   /* hlt                     */
        0x40,   /* inc ax -- reached only if woken */
        0xF4,   /* hlt                     */
    };

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);

    cpu->ax = 0;
    vm86_raise(cpu, 0x08);

    vm86_run(cpu, 20);

    vm86_expect_bool("the interrupt was delivered", g_log_len == 1, true);
    vm86_expect_u16("the machine carried on past the HLT", cpu->ax, 1);
    vm86_expect_bool("and stopped at the second one", cpu->halted, true);
}

/*
 * `cli; hlt` is the end, because nothing can wake it.
 *
 * A pending interrupt is raised on purpose and must not be delivered: IF
 * is clear, and a maskable interrupt cannot wake a processor that is not
 * listening for one. This is how a program in this milestone ends, so a
 * machine that woke here would be a machine where no program ever
 * terminates.
 */
static void test_cli_hlt_is_terminal(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xFA,   /* cli */
        0xF4,   /* hlt */
    };

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);

    vm86_raise(cpu, 0x08);   /* IF is clear out of reset already */

    enum vm86_stop stop = vm86_run(cpu, 20);

    vm86_expect_bool("the machine stopped",
                     stop == VM86_STOP_HALT, true);
    vm86_expect_bool("and the pending interrupt was not delivered",
                     g_log_len == 0, true);
}

/* ------------------------------------------------------------------ */
/* The budget                                                          */
/* ------------------------------------------------------------------ */

/*
 * A budget of zero executes nothing and still delivers.
 *
 * "Is anything waiting that should run" is a question a host asks before
 * it decides to spend a slice, and it is the reason the delivery step
 * sits above the budget check rather than below it. Put the budget first
 * and this call becomes a no-op that answers nothing.
 *
 * What "delivered" looks like when no instruction may run is worth being
 * exact about, because it is the whole distinction the ordering turns on:
 * the frame is on the stack and the machine is standing on the stub, and
 * the service has *not* been called, because calling it takes an
 * instruction. Delivery is not charged to the budget; executing the stub
 * is. So the service is asserted to run on the next slice, with a budget
 * that allows one.
 */
static void test_a_budget_of_zero_still_delivers(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };   /* hlt */

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);
    interrupt_enable(cpu);
    vm86_raise(cpu, 0x08);

    uint16_t sp_before = cpu->sp;

    enum vm86_stop stop = vm86_run(cpu, 0);

    vm86_expect_u16("no instruction retired", (uint16_t)cpu->insn_count, 0);
    vm86_expect_bool("and the slice ended on the budget",
                     stop == VM86_STOP_STEPS, true);

    vm86_expect_u16("the interrupt frame is on the stack", cpu->sp,
                    (uint16_t)(sp_before - 6));
    vm86_expect_u16("the machine is standing on the stub", cpu->ip,
                    VM86_TRAP_BASE + (0x08u * VM86_TRAP_STRIDE));
    vm86_expect_u16("in the stub's segment", cpu->cs, VM86_TRAP_SEGMENT);
    vm86_expect_bool("and the vector is no longer pending",
                     vm86_next_pending(cpu) < 0, true);
    vm86_expect_bool("the service has not been called yet", g_log_len == 0,
                     true);

    /* Given a budget that allows one instruction, the stub runs and the
     * service with it. */
    vm86_run(cpu, 4);

    vm86_expect_u16("now the service runs", (uint16_t)g_log_len, 1);
    vm86_expect_u16("on vector 08h", g_log[0], 0x08);
}

/*
 * A slice that ends inside the grace period does not spend it.
 *
 * The grace is one *instruction*, and it belongs to the instruction that
 * follows the STI. Spending it at a boundary where the budget then stops
 * the machine would leave the shadow gone and nothing executed, and the
 * next slice would deliver an interrupt in the middle of the window --
 * for `sti; hlt` that means the machine is woken by an interrupt that the
 * HLT then waits for a second time, and an idle loop stops rather than
 * sleeping. It is the same failure the grace exists to prevent, arriving
 * by a different route.
 *
 * The first run is given a budget of one instruction, which is exactly
 * the STI. The shadow has to still be there afterwards.
 */
static void test_a_slice_does_not_spend_the_grace_it_cannot_use(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xFB,   /* sti */
        0xF4,   /* hlt */
        0x40,   /* inc ax -- reached only if woken */
        0xF4,   /* hlt */
    };

    machine(cpu, code, sizeof(code));
    vm86_register_service(0x08, recording_service, (void *)(uintptr_t)0x08);

    cpu->ax = 0;
    vm86_raise(cpu, 0x08);

    enum vm86_stop first = vm86_run(cpu, 1);

    vm86_expect_bool("the slice ended on the budget",
                     first == VM86_STOP_STEPS, true);
    vm86_expect_u16("the grace is still unspent", cpu->intr_shadow, 1);

    vm86_run(cpu, 20);

    vm86_expect_bool("the interrupt was delivered", g_log_len == 1, true);
    vm86_expect_u16("the machine carried on past the HLT", cpu->ax, 1);
}

/* ------------------------------------------------------------------ */
/* Faults, and where they go                                           */
/* ------------------------------------------------------------------ */

/*
 * An exception nobody hooked stops the machine.
 *
 * This is what M3 did with every fault and what the whole existing suite
 * expects, so it is the behaviour that must not change. Vector 0 is
 * pointed at the firmware's stub, which means no handler was ever
 * installed for it, which means there is nowhere to deliver it -- and
 * stopping with the vector left in cpu->fault is what a caller can report
 * on.
 */
static void test_an_unhooked_fault_stops_the_machine(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xF6, 0xF1,   /* div cl */
        0xF4,         /* hlt    */
    };

    machine(cpu, code, sizeof(code));
    cpu->cx = 0x0000;   /* divide by zero */

    enum vm86_stop stop = vm86_run(cpu, 20);

    vm86_expect_bool("the machine stopped on the fault",
                     stop == VM86_STOP_FAULT, true);
    vm86_expect_u16("and said which vector", cpu->fault,
                    VM86_INT_DIVIDE_ERROR);
}

/*
 * An exception the guest did hook is delivered to it.
 *
 * Same instruction, same vector, one difference: the guest overwrote
 * IVT[0]. That is the whole rule -- the machine compares the vector
 * against the stub it installed rather than watching writes to the first
 * kilobyte of memory, which is the same answer for a good deal less
 * machinery.
 *
 * The handler returns with IRET, so execution resumes at the instruction
 * after the one that faulted, and the byte after it in the program is
 * there to prove it: `inc ax` runs only if the fault round-tripped.
 */
static void test_a_hooked_fault_is_delivered_to_the_guest(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xF6, 0xF1,   /* div cl -- faults */
        0x40,         /* inc ax           */
        0xF4,         /* hlt              */
    };

    /* inc word [SCRATCH] ; iret */
    static const uint8_t handler[] = {
        0xFF, 0x06, (uint8_t)(SCRATCH & 0xFF), (uint8_t)(SCRATCH >> 8),
        0xCF,
    };

    machine(cpu, code, sizeof(code));
    place(cpu, HANDLER, handler, sizeof(handler));
    hook(cpu, VM86_INT_DIVIDE_ERROR, HANDLER);

    cpu->cx = 0x0000;
    cpu->ax = 0;

    vm86_run(cpu, 20);

    vm86_expect_u16("the guest's handler ran", vm86_mem_read16(
                        cpu->mem, SCRATCH), 1);
    vm86_expect_u16("and execution resumed after the fault", cpu->ax, 1);
    vm86_expect_bool("halted", cpu->halted, true);
}

/* ------------------------------------------------------------------ */
/* A repeated string instruction is interruptible                      */
/* ------------------------------------------------------------------ */

/*
 * `rep movsb` stopped partway and resumed where it left off.
 *
 * This is the case that the language-lawyering is about, so it is worth
 * being precise about what it measures.
 *
 * The string loop runs its body, decrements CX, and *then* looks for an
 * interrupt. That position is the whole of it. Checking before the body
 * would let an interrupt that is already pending stop the instruction
 * before it had copied anything -- which is not what the hardware does,
 * and which would make the claim that CX, SI and DI are "not rewound"
 * untestable, because an instruction that never ran has nothing to not
 * rewind. So the first element always moves, and this case measures the
 * registers from inside the handler to prove that the second element was
 * where it should be.
 *
 * The rewind target is the other half. `cpu->insn_ip` is the start of the
 * instruction with its prefixes, and the frame records it, so the REP is
 * re-decoded when execution resumes. Rewinding to just past the F3
 * instead drops the prefix: the handler returns, `movsb` runs once, and
 * the machine copies one byte of an eight-byte buffer while looking
 * entirely healthy. The bytes at the destination are the assertion for
 * that, and SI and CX from inside the handler are the assertion for the
 * rest.
 */
static void test_rep_is_interrupted_between_iterations(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xFB,         /* sti                        */
        0xF3, 0xA4,   /* rep movsb                  */
        0xF4,         /* hlt                        */
    };

    /* inc word [SCRATCH] ; mov [SCRATCH+2], si ; mov [SCRATCH+4], cx ; iret */
    static const uint8_t handler[] = {
        0xFF, 0x06, (uint8_t)(SCRATCH & 0xFF),     (uint8_t)(SCRATCH >> 8),
        0x89, 0x36, (uint8_t)((SCRATCH + 2) & 0xFF), (uint8_t)((SCRATCH + 2) >> 8),
        0x89, 0x0E, (uint8_t)((SCRATCH + 4) & 0xFF), (uint8_t)((SCRATCH + 4) >> 8),
        0xCF,
    };

    machine(cpu, code, sizeof(code));
    place(cpu, HANDLER, handler, sizeof(handler));
    hook(cpu, 0x08, HANDLER);

    for (uint16_t i = 0; i < 8; i++) {
        poke8(cpu, SOURCE + i, (uint8_t)(0x11 + i));
        poke8(cpu, DEST + i, 0x00);
    }

    cpu->si = SOURCE;
    cpu->di = DEST;
    cpu->cx = 8;

    vm86_raise(cpu, 0x08);

    vm86_run(cpu, 60);

    for (uint16_t i = 0; i < 8; i++) {
        vm86_expect_mem8("every byte was copied", cpu, DEST + i,
                         (uint8_t)(0x11 + i));
    }

    vm86_expect_u16("the handler ran once",
                    vm86_mem_read16(cpu->mem, SCRATCH), 1);
    vm86_expect_u16("SI had already advanced by one element",
                    vm86_mem_read16(cpu->mem, SCRATCH + 2), SOURCE + 1);
    vm86_expect_u16("and CX counted the elements that were left",
                    vm86_mem_read16(cpu->mem, SCRATCH + 4), 7);
}

/*
 * The same loop, reached through INS.
 *
 * INS and OUTS do not inherit REP by accident: they call the same
 * vm86_str_repeat() that MOVS does, with their own body. That is
 * deliberate -- two copies of the loop would not stay equal, and the
 * failure would be a machine where `rep movsb` works and `rep insb`
 * moves one byte.

 * Which is exactly why this case exists. The check added for the REP debt
 * is inside the shared loop, so it is dead code for INS/OUTS if the loop
 * is not actually shared, and no MOVS case can tell the difference.
 */
static void test_rep_ins_is_interrupted_between_iterations(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xBA, 0x60, 0x00,   /* mov dx, 0060h -- the keyboard's data port */
        0xCD, 0x80,         /* int 80h -- arms the interrupt             */
        0xFB,               /* sti                                       */
        0xF3, 0x6C,         /* rep insb                                  */
        0xF4,               /* hlt                                       */
    };

    /* inc word [SCRATCH] ; mov [SCRATCH+2], di ; mov [SCRATCH+4], cx ; iret */
    static const uint8_t handler[] = {
        0xFF, 0x06, (uint8_t)(SCRATCH & 0xFF),     (uint8_t)(SCRATCH >> 8),
        0x89, 0x3E, (uint8_t)((SCRATCH + 2) & 0xFF), (uint8_t)((SCRATCH + 2) >> 8),
        0x89, 0x0E, (uint8_t)((SCRATCH + 4) & 0xFF), (uint8_t)((SCRATCH + 4) >> 8),
        0xCF,
    };

    machine(cpu, code, sizeof(code));
    place(cpu, HANDLER, handler, sizeof(handler));
    hook(cpu, 0x08, HANDLER);

    vm86_register_service(0x80, arming_service, (void *)(uintptr_t)0x08);

    for (uint16_t i = 0; i < 4; i++)
        poke8(cpu, DEST + i, 0x00);

    cpu->di = DEST;
    cpu->cx = 4;

    vm86_run(cpu, 60);

    /* No device answers port 0060h, so a read gives back what a floating
     * bus gives back -- and the point here is only that the loop ran to
     * the end and resumed in the right place. */
    for (uint16_t i = 0; i < 4; i++)
        vm86_expect_mem8("every byte was read in", cpu, DEST + i, 0xFF);

    vm86_expect_u16("the handler ran once",
                    vm86_mem_read16(cpu->mem, SCRATCH), 1);
    vm86_expect_u16("DI had already advanced by one element",
                    vm86_mem_read16(cpu->mem, SCRATCH + 2), DEST + 1);
    vm86_expect_u16("and CX counted the elements that were left",
                    vm86_mem_read16(cpu->mem, SCRATCH + 4), 3);
}

/* ------------------------------------------------------------------ */
/* The firmware called through the trap                                */
/* ------------------------------------------------------------------ */

/*
 * INT 11h and INT 12h answer through the vector table.
 *
 * Both are registered by vm86_install_firmware() and both reach the host
 * the same way any other service does, so this is really a check that the
 * machine describes itself to a guest at all: a program that asks how much
 * memory there is gets the number the BIOS data area holds, and one that
 * asks what equipment is attached gets the equipment word.
 *
 * The two are asserted against the data area rather than against
 * constants. That is the point of the services reading the area instead
 * of carrying their own copy: there is one place each fact lives, and a
 * service that hard-coded its answer could disagree with the area a
 * program reads directly, which is the failure mode firmware.h warns
 * about.
 */
static void test_the_machine_describes_itself(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xCD, 0x12,         /* int 12h -- memory size */
        0x89, 0xC3,         /* mov bx, ax             */
        0xCD, 0x11,         /* int 11h -- equipment   */
        0xF4,               /* hlt                    */
    };

    machine(cpu, code, sizeof(code));

    vm86_run(cpu, 20);

    uint16_t memory_word = vm86_mem_read16(
        cpu->mem, (VM86_BDA_SEGMENT << 4) + VM86_BDA_MEMORY_KB);
    uint16_t equipment_word = vm86_mem_read16(
        cpu->mem, (VM86_BDA_SEGMENT << 4) + VM86_BDA_EQUIPMENT);

    vm86_expect_u16("INT 12h answers what the data area holds",
                    cpu->bx, memory_word);
    vm86_expect_u16("INT 11h answers what the data area holds",
                    cpu->ax, equipment_word);

    vm86_expect_bool("and the memory word is a plausible size",
                     memory_word == 640, true);
}

/*
 * The stub is there, and it is recognisable.
 *
 * vm86_vector_is_stub() is what the run loop asks before deciding whether
 * an exception has anywhere to go, so its two answers are both worth
 * pinning: true while the vector still points at the firmware's stub, and
 * false the moment a program takes the vector over. The third answer --
 * a vector a program took over and then handed back, by writing the old
 * address into it -- is true again, and that is correct rather than a
 * gap: it really is pointing at the stub.
 */
static void test_the_stub_is_recognisable(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = { 0xF4 };

    machine(cpu, code, sizeof(code));

    vm86_expect_bool("an untouched vector is ours",
                     vm86_vector_is_stub(cpu, 0x10), true);
    vm86_expect_bool("and so is one with no service",
                     vm86_vector_is_stub(cpu, 0x33), true);

    hook(cpu, 0x10, HANDLER);

    vm86_expect_bool("a vector the guest took is not",
                     vm86_vector_is_stub(cpu, 0x10), false);

    /* Handing it back by writing the stub's own address into it is what a
     * TSR does on the way out, and the answer is true -- it is pointed at
     * the stub. */
    poke16(cpu, 0x10u * 4u,
           VM86_TRAP_BASE + (0x10u * VM86_TRAP_STRIDE));
    poke16(cpu, (0x10u * 4u) + 2u, VM86_TRAP_SEGMENT);

    vm86_expect_bool("and one handed back is ours again",
                     vm86_vector_is_stub(cpu, 0x10), true);
}

/* ------------------------------------------------------------------ */
/* A blocking read                                                     */
/* ------------------------------------------------------------------ */

/*
 * A service that cannot answer yet asks for the instruction to run again,
 * and the interrupt that makes the answer possible is delivered in
 * between.
 *
 * This is the mechanism behind every blocking BIOS call, and the shape of
 * it is worth stating because it is not obvious: a service cannot wait.
 * It is called from the interpreter, so a service that spun would never
 * give the host its processor back, and the keystroke it was waiting for
 * could never be fed in. So the wait is moved outside -- the service
 * returns having done nothing, the instruction it was reached through is
 * re-run, and the host gets a look in at every instruction boundary in
 * between.
 *
 * For the host to have anything to deliver at those boundaries, the guest
 * has to be *listening*: the INT cleared IF on entry, and if the retry
 * left it cleared then no hardware interrupt can ever be delivered
 * between one attempt and the next, and the program waits forever. That
 * is the difference this case measures, and it is measured rather than
 * reasoned about: the first slice runs with nothing pending, the key
 * arrives between the slices, and the second slice either delivers it or
 * does not.
 */
static void test_a_blocking_read_waits_with_interrupts_live(struct vm86_cpu *cpu)
{
    static const uint8_t code[] = {
        0xCD, 0x16,   /* int 16h -- read a keystroke */
        0xF4,         /* hlt                         */
    };

    machine(cpu, code, sizeof(code));

    g_key_ready = 0;
    g_reads     = 0;

    vm86_register_service(0x16, blocking_read_service, NULL);
    vm86_register_service(0x09, keyboard_service, NULL);

    interrupt_enable(cpu);

    /* Nothing has arrived yet, so the read refuses and gives the host its
     * processor back. */
    vm86_run(cpu, 32);

    vm86_expect_bool("the read was tried", g_reads > 0, true);
    vm86_expect_bool("and nothing was answered yet", g_key_ready == 0, true);

    /* Between the slices the key arrives, which is the only thing a host
     * can do about a blocking read: feed the device and raise its
     * interrupt. */
    vm86_raise(cpu, 0x09);

    vm86_run(cpu, 32);

    vm86_expect_bool("the keyboard interrupt was delivered between attempts",
                     g_key_ready, true);
    vm86_expect_u16("and the read got its answer", cpu->al, 0x41);
    vm86_expect_bool("halted", cpu->halted, true);
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

static const struct vm86_test tests[] = {
    { "a trap call comes back",         test_a_trap_call_comes_back },
    { "the stub chooses the service",   test_the_stub_chooses_the_service },
    { "an unregistered vector is not an error",
      test_an_unregistered_vector_is_not_an_error },

    { "a vector raised twice is pending once",
      test_a_vector_raised_twice_is_pending_once },
    { "the lowest pending vector is delivered first",
      test_the_lowest_pending_vector_is_delivered_first },

    { "sti delays recognition by one instruction",
      test_sti_delays_recognition_by_one_instruction },
    { "mov ss delays recognition by one instruction",
      test_mov_ss_delays_recognition_by_one_instruction },
    { "pop ss delays recognition by one instruction",
      test_pop_ss_delays_recognition_by_one_instruction },

    { "sti; hlt is woken rather than stopped",
      test_sti_hlt_is_woken_rather_than_stopped },
    { "cli; hlt is terminal",           test_cli_hlt_is_terminal },

    { "a budget of zero still delivers",
      test_a_budget_of_zero_still_delivers },
    { "a slice does not spend the grace it cannot use",
      test_a_slice_does_not_spend_the_grace_it_cannot_use },

    { "an unhooked fault stops the machine",
      test_an_unhooked_fault_stops_the_machine },
    { "a hooked fault is delivered to the guest",
      test_a_hooked_fault_is_delivered_to_the_guest },

    { "rep is interrupted between iterations",
      test_rep_is_interrupted_between_iterations },
    { "rep ins is interrupted between iterations",
      test_rep_ins_is_interrupted_between_iterations },

    { "the machine describes itself",   test_the_machine_describes_itself },
    { "the stub is recognisable",       test_the_stub_is_recognisable },

    { "a blocking read waits with interrupts live",
      test_a_blocking_read_waits_with_interrupts_live },
};

VM86_TEST_MAIN("intr", tests)
