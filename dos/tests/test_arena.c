/* MCB ownership and TSR boundary tests.  These are deliberately direct host
 * tests: they exercise the guest-visible arena without depending on a boot
 * image or on a launcher policy. */
#include "harness.h"
#include <vm86/dos.h>
#include <vm86/int21.h>
#include <vm86/mem.h>

#define PSP 0x1000u

static void setup(struct vm86_cpu *cpu, struct int21_state *dos)
{
    vm86_mem_clear(cpu->mem);
    vm86_reset(cpu, cpu->mem);
    int21_reset(dos, NULL, PSP);
    dos_runtime_init(cpu, dos);
}

static void allocate_free_merge(struct vm86_cpu *cpu)
{
    struct int21_state dos;
    setup(cpu, &dos);

    uint16_t largest = 0, first = 0, second = 0, size = 0;
    vm86_expect_u16("shrink owner", dos_memory_resize(cpu, &dos, PSP, 0x100,
                                                       &largest), 0);
    vm86_expect_u16("first allocation", dos_memory_alloc(cpu, &dos, 0x20,
                                                          PSP, &first, &largest), 0);
    vm86_expect_u16("second allocation", dos_memory_alloc(cpu, &dos, 0x20,
                                                           PSP, &second, &largest), 0);
    vm86_expect_bool("allocations are distinct", first != second, true);
    vm86_expect_u16("first block size", dos_memory_block_size(cpu, &dos, first,
                                                                &size), 0);
    vm86_expect_u16("first block paragraphs", size, 0x20);

    vm86_expect_u16("free first", dos_memory_free(cpu, &dos, first), 0);
    vm86_expect_u16("free second", dos_memory_free(cpu, &dos, second), 0);
    vm86_expect_u16("merged free allocation", dos_memory_alloc(cpu, &dos, 0x40,
                                                                 PSP, &first, &largest), 0);
    vm86_expect_u16("merged block size", dos_memory_block_size(cpu, &dos, first,
                                                                 &size), 0);
    vm86_expect_u16("merged size", size, 0x40);
}

static void invalid_and_oom(struct vm86_cpu *cpu)
{
    struct int21_state dos;
    setup(cpu, &dos);

    uint16_t largest = 0, block = 0;
    vm86_expect_u16("small owner", dos_memory_resize(cpu, &dos, PSP, 0x20,
                                                      &largest), 0);
    vm86_expect_u16("out of memory", dos_memory_alloc(cpu, &dos, 0x8fe0, PSP,
                                                       &block, &largest), 8);
    vm86_expect_u16("largest free result", largest, 0x8FDF);

    /* A damaged MCB must not be followed as a pointer or edited. */
    vm86_mem_write8(cpu->mem, ((uint32_t)(PSP - 1u) << 4), 'X');
    vm86_expect_u16("corrupt chain refusal", dos_memory_alloc(cpu, &dos, 1,
                                                               PSP, &block,
                                                               &largest), 7);
}

static void tsr_reserves_exact_block(struct vm86_cpu *cpu)
{
    struct int21_state dos;
    setup(cpu, &dos);
    uint16_t largest = 0;
    vm86_expect_u16("prepare TSR block", dos_memory_resize(cpu, &dos, PSP, 0x80,
                                                            &largest), 0);

    cpu->ah = 0x31;
    cpu->al = 7;
    cpu->dx = 0x20;
    vm86_flag_set(cpu, VM86_CF, true);
    dos_misc_call(cpu, &dos);
    vm86_expect_bool("TSR exits", cpu->exited, true);
    vm86_expect_bool("TSR marked resident", dos.terminated_resident, true);
    vm86_expect_u16("TSR code", dos.last_exit_code, 7);
    vm86_expect_u16("TSR PSP top", vm86_mem_read16(cpu->mem,
                                                     ((uint32_t)PSP << 4) + VM86_PSP_MEMORY_TOP),
                     PSP + 0x20);
    vm86_expect_u16("TSR MCB size", vm86_mem_read16(cpu->mem,
                                                      ((uint32_t)(PSP - 1u) << 4) + 3),
                     0x20);
}

static void tsr_rejects_overflow(struct vm86_cpu *cpu)
{
    struct int21_state dos;
    setup(cpu, &dos);
    uint16_t before = vm86_mem_read16(cpu->mem,
                                      ((uint32_t)PSP << 4) + VM86_PSP_MEMORY_TOP);
    cpu->ah = 0x31;
    cpu->al = 1;
    cpu->dx = 0xffff;
    dos_misc_call(cpu, &dos);
    vm86_expect_bool("oversized TSR does not exit", cpu->exited, false);
    vm86_expect_flag("oversized TSR CF", cpu, VM86_CF, true);
    vm86_expect_u16("oversized TSR error", cpu->ax, 8);
    vm86_expect_u16("oversized TSR keeps PSP top",
                     vm86_mem_read16(cpu->mem, ((uint32_t)PSP << 4) + VM86_PSP_MEMORY_TOP),
                     before);
}

static const struct vm86_test tests[] = {
    { "allocate, free and coalesce", allocate_free_merge },
    { "invalid chain and out of memory", invalid_and_oom },
    { "TSR reserves exact MCB block", tsr_reserves_exact_block },
    { "TSR rejects oversized request", tsr_rejects_overflow },
};
VM86_TEST_MAIN("arena", tests)
