/* Host-side INT 21h/AH=4Bh contract tests.  The real VM callback is tested by
 * the ISO runner; this suite pins the dispatcher ABI and all failure returns. */
#include "harness.h"
#include <vm86/int21.h>

static unsigned calls;
static uint8_t seen_mode;
static uint16_t seen_segment, seen_offset;

static int callback(struct vm86_cpu *cpu, struct int21_state *st,
                    uint8_t mode, uint16_t segment, uint16_t offset,
                    uint16_t *code)
{
    (void)cpu; (void)st;
    calls++;
    seen_mode = mode;
    seen_segment = segment;
    seen_offset = offset;
    if (mode == 2u) return VM86_INT21_ERR_ACCESS;
    if (code) *code = 0x42;
    return 0;
}

static void setup(struct vm86_cpu *cpu, struct int21_state *dos)
{
    vm86_mem_clear(cpu->mem);
    vm86_reset(cpu, cpu->mem);
    int21_reset(dos, NULL, 0x1000);
    dos->full_services = true;
    dos->exec_request = callback;
    calls = 0;
}

static void successful_exec_and_return_code(struct vm86_cpu *cpu)
{
    struct int21_state dos;
    setup(cpu, &dos);
    cpu->al = 0;
    cpu->es = 0x2345;
    cpu->bx = 0x6789;
    cpu->ah = 0x4b;
    int21_service(cpu, &dos);
    vm86_expect_u16("EXEC callback count", calls, 1);
    vm86_expect_u16("EXEC mode", seen_mode, 0);
    vm86_expect_u16("EXEC parameter segment", seen_segment, 0x2345);
    vm86_expect_u16("EXEC parameter offset", seen_offset, 0x6789);
    vm86_expect_flag("EXEC clears CF", cpu, VM86_CF, false);

    cpu->ah = 0x4d;
    int21_service(cpu, &dos);
    vm86_expect_u16("EXEC return code", cpu->al, 0x42);
}

static void reject_mode_and_callback_failure(struct vm86_cpu *cpu)
{
    struct int21_state dos;
    setup(cpu, &dos);

    cpu->ah = 0x4b;
    cpu->al = 4;
    int21_service(cpu, &dos);
    vm86_expect_u16("invalid EXEC mode", cpu->ax, VM86_INT21_ERR_FUNCTION);
    vm86_expect_flag("invalid EXEC mode CF", cpu, VM86_CF, true);
    vm86_expect_u16("invalid mode does not call callback", calls, 0);

    cpu->ah = 0x4b;
    cpu->al = 2;
    int21_service(cpu, &dos);
    vm86_expect_u16("child failure code", cpu->ax, VM86_INT21_ERR_ACCESS);
    vm86_expect_flag("child failure CF", cpu, VM86_CF, true);
    vm86_expect_u16("child failure callback count", calls, 1);
}

static const struct vm86_test tests[] = {
    { "EXEC callback and child return code", successful_exec_and_return_code },
    { "EXEC mode and child failure", reject_mode_and_callback_failure },
};
VM86_TEST_MAIN("exec", tests)
