/*
 * A test harness for the 8086 core.
 *
 * ---------------------------------------------------------------------
 * Why these run on the host
 *
 * The interpreter is pure logic. It has no idea it is inside an operating
 * system: give it a block of memory and some bytes and it will execute
 * them. So its tests do not need this operating system, do not need a
 * virtual machine, and do not need to boot anything.
 *
 * That matters more than it sounds. A test that takes twenty seconds to
 * run is a test nobody runs while thinking; one that takes a millisecond
 * is one you run after every three lines. This whole directory exists so
 * that getting an instruction right is a thing you can check immediately.
 *
 * Build and run:
 *
 *     make -C dos test          all suites
 *     make -C dos test-alu      one suite
 *
 * ---------------------------------------------------------------------
 * Writing a test
 *
 * Each suite is its own program with its own main(), so no two people
 * editing the tests ever touch the same file. A test is a function that
 * receives a CPU with guest memory already attached:
 *
 *     static void test_add_al_imm8(struct vm86_cpu *cpu)
 *     {
 *         static const uint8_t code[] = { 0x04, 0x05 };   // add al, 5
 *
 *         vm86_test_load(cpu, code, sizeof(code));
 *         cpu->ax = 0x0010;
 *         vm86_test_run(cpu, 10);
 *
 *         vm86_expect_u16("AL", cpu->al, 0x15);
 *         vm86_expect_flag("CF", cpu, VM86_CF, false);
 *     }
 *
 * and then listed at the bottom of the file:
 *
 *     static const struct vm86_test tests[] = {
 *         { "add al, imm8", test_add_al_imm8 },
 *     };
 *     VM86_TEST_MAIN("alu", tests)
 *
 * ---------------------------------------------------------------------
 * On what to assert
 *
 * Assert the flags. An instruction that produces the right value with the
 * wrong carry flag is wrong, and it will not be caught by a test that
 * only looks at the value -- the program will simply take the other
 * branch somewhere much later, and the bug will be blamed on whatever is
 * there when it happens.
 *
 * Prefer exact values in binary. Where a result is inexact, pick operands
 * that make it exact; a test that needs a tolerance is a test that cannot
 * tell a bug from rounding.
 */
#ifndef VM86_TEST_HARNESS_H
#define VM86_TEST_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/decode.h>
#include <vm86/mem.h>
#include <vm86/ops.h>

/*
 * Where test code is loaded, and how much guest memory a test gets.
 *
 * The address is the one DOS uses for a .COM program's entry point, so
 * the bytes in a test look like the bytes in a real program. The memory
 * is a bit over the 640K of a conventional DOS machine, which is enough
 * for every test here and small enough that a runaway program hits the
 * end rather than eating the host.
 */
#define VM86_TEST_CODE_BASE 0x0100u
#define VM86_TEST_STACK_TOP 0xFFFEu
#define VM86_TEST_MEMORY    (1u * 1024u * 1024u)

typedef void (*vm86_test_fn)(struct vm86_cpu *cpu);

struct vm86_test {
    const char  *name;
    vm86_test_fn fn;
};

/*
 * Give each suite its own program.
 *
 * Separate binaries rather than one with a shared registry, so that two
 * people adding tests never edit the same file -- and so that one broken
 * suite does not stop the others from running.
 */
int vm86_test_main(const char *suite, const struct vm86_test *tests,
                   size_t count);

#define VM86_TEST_MAIN(suite, tests)                                 \
    int main(void)                                                   \
    {                                                                \
        return vm86_test_main(suite, tests,                          \
                              sizeof(tests) / sizeof((tests)[0]));   \
    }

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

/*
 * Copy `code` into guest memory at VM86_TEST_CODE_BASE, point CS:IP at
 * it, and set SP to the top of the stack.
 *
 * The machine has already been reset by the harness, so memory is zero
 * and the general purpose registers are zero. Set up the registers this
 * test needs *after* calling this, not before.
 */
void vm86_test_load(struct vm86_cpu *cpu, const uint8_t *code, uint16_t size);

/*
 * Run until the machine halts, faults, or `limit` instructions have
 * executed.
 *
 * The limit is not optional. A test with a bug in a loop will otherwise
 * hang, and a hung test tells you less than a failed one.
 *
 * EVERY TEST'S CODE MUST END IN HLT. There is no end of program on an
 * 8086: when IP runs past the last byte the processor keeps fetching, and
 * guest memory is zeroed, so it decodes a run of 00 bytes -- which is
 * `add [bx+si], al`, four times over, clobbering flags and writing to
 * address zero. A test whose code stops without halting therefore
 * measures the harness's leftovers rather than its own instruction, and
 * it does so in a way that passes or fails depending on what the
 * instruction under test happened to leave behind. This has already
 * caught one test written by the person who wrote the harness.
 *
 * The instruction limit is the backstop, not the contract. A test that
 * relies on it is a test that is running the wrong program.
 */
enum vm86_result vm86_test_run(struct vm86_cpu *cpu, uint64_t limit);

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */

/*
 * Each of these records a failure and prints it, and none of them stops
 * the test. One run should report everything that is wrong rather than
 * the first thing -- otherwise fixing four bugs takes four runs.
 */

void vm86_expect_u16(const char *what, uint16_t got, uint16_t want);

void vm86_expect_bool(const char *what, bool got, bool want);

/* One flag, checked by name. */
void vm86_expect_flag(const char *what, const struct vm86_cpu *cpu,
                      uint16_t flag, bool want);

/*
 * The whole flags register.
 *
 * `expected` is the set of flags that should be set; every other writable
 * flag must be clear. Bits passed in `ignore` are not examined, which is
 * how a test deals with a flag the manual leaves undefined after an
 * instruction -- AF after a logical operation, for instance.
 *
 * The bits that are always set are masked off and never reported.
 */
void vm86_expect_flags(const char *what, const struct vm86_cpu *cpu,
                       uint16_t expected, uint16_t ignore);

/* Raw guest memory, for the tests that check what an instruction stored
 * rather than what it computed. */
uint8_t  vm86_expect_mem8 (const char *what, struct vm86_cpu *cpu,
                           uint32_t linear, uint8_t want);
uint16_t vm86_expect_mem16(const char *what, struct vm86_cpu *cpu,
                           uint32_t linear, uint16_t want);

#endif /* VM86_TEST_HARNESS_H */
