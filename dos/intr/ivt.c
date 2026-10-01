/*
 * The interrupt vector table and the stubs it points at.
 *
 * Every one of the 256 vectors gets a stub, including the ones with no
 * service. That is not defensive programming: it is what firmware does.
 * A machine whose table is full of zeroes runs an unexpected interrupt
 * off into whatever is at address zero, and a program that hooks a vector
 * it never expects to fire gets a machine that behaves differently from
 * the one it was written for. An IRET stub makes the same interrupt
 * harmless.
 *
 * See the top of host.h for the stub's encoding and why it is that one.
 */
#include <vm86/host.h>

/* Where a vector's stub lives inside the stub area. */
static uint16_t stub_offset(uint8_t vector)
{
    return (uint16_t)((uint32_t)vector * VM86_TRAP_STRIDE);
}

/* Where a vector's entry lives in the table at the bottom of memory. */
static uint32_t vector_entry(uint8_t vector)
{
    return (uint32_t)vector * 4u;
}

static void write_stub(struct vm86_cpu *cpu, uint8_t vector, uint16_t offset)
{
    uint32_t at = VM86_TRAP_LINEAR + offset;

    vm86_mem_write8(cpu->mem, at,      VM86_TRAP_OPCODE);
    vm86_mem_write8(cpu->mem, at + 1u, VM86_TRAP_MODRM);

    /* The vector, so that the trap knows which service is being asked
     * for without having to work back from its own address. */
    vm86_mem_write8(cpu->mem, at + 2u, vector);

    vm86_mem_write8(cpu->mem, at + 3u, VM86_TRAP_IRET);
}

void vm86_install_ivt(struct vm86_cpu *cpu)
{
    for (uint32_t v = 0; v < VM86_TRAP_VECTORS; v++) {
        uint8_t  vector = (uint8_t)v;
        uint16_t offset = stub_offset(vector);

        write_stub(cpu, vector, offset);

        vm86_mem_write16(cpu->mem, vector_entry(vector), offset);
        vm86_mem_write16(cpu->mem, vector_entry(vector) + 2u,
                         VM86_TRAP_SEGMENT);
    }
}

bool vm86_vector_is_stub(const struct vm86_cpu *cpu, uint8_t vector)
{
    uint32_t entry = vector_entry(vector);

    uint16_t offset  = vm86_mem_read16(cpu->mem, entry);
    uint16_t segment = vm86_mem_read16(cpu->mem, entry + 2u);

    return offset == stub_offset(vector) && segment == VM86_TRAP_SEGMENT;
}
