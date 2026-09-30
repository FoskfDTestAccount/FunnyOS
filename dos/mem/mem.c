#include <vm86/mem.h>

#include <libk/string.h>

/*
 * The 8086 has twenty address lines, so with the A20 gate closed a linear
 * address wraps at 1 MiB. See the note in mem.h for why the gate is
 * modelled at all.
 */
#define VM86_A20_MASK 0x000FFFFFu

void vm86_mem_attach(struct vm86_mem *mem, uint8_t *ram, uint32_t size)
{
    mem->ram            = ram;
    mem->size           = size;
    mem->a20            = true;
    mem->unmapped_reads = 0;
    mem->dropped_writes = 0;
}

void vm86_mem_clear(struct vm86_mem *mem)
{
    if (mem->ram && mem->size)
        memset(mem->ram, 0, mem->size);

    mem->unmapped_reads = 0;
    mem->dropped_writes = 0;
}

uint32_t vm86_mem_offset(struct vm86_mem *mem, uint32_t linear)
{
    if (!mem->a20)
        linear &= VM86_A20_MASK;

    /* Past the end of the region is not an error and not a wrap: it is
     * an address with nothing behind it. See the header. */
    if (linear >= mem->size)
        return VM86_MEM_UNMAPPED;

    return linear;
}

/*
 * Accessors.
 *
 * Every one of these is a candidate for inlining and none of them is
 * complicated, which is the point -- the interesting behaviour is in
 * vm86_mem_offset, and everything else is a load or a store.
 */

uint8_t vm86_mem_read8(struct vm86_mem *mem, uint32_t linear)
{
    uint32_t offset = vm86_mem_offset(mem, linear);

    if (offset == VM86_MEM_UNMAPPED) {
        /* A floating bus reads as all ones. Programs of the era probed for
         * the absence of a device this way, so returning zero here would
         * break software that worked on the hardware. */
        mem->unmapped_reads++;
        return 0xFF;
    }

    return mem->ram[offset];
}

void vm86_mem_write8(struct vm86_mem *mem, uint32_t linear, uint8_t value)
{
    uint32_t offset = vm86_mem_offset(mem, linear);

    if (offset == VM86_MEM_UNMAPPED) {
        mem->dropped_writes++;
        return;
    }

    mem->ram[offset] = value;
}

uint16_t vm86_mem_read16(struct vm86_mem *mem, uint32_t linear)
{
    /*
     * Two byte accesses rather than one 16-bit load.
     *
     * It costs a compare that the branch predictor will get right every
     * time, and it buys correctness at the one place that is otherwise
     * awkward: an access that starts in the last byte of the region.
     * Reading `*(uint16_t *)&ram[size - 1]` would run off the end of the
     * host's array -- reading memory the host owns and the guest does
     * not, which is exactly the boundary this whole layer exists to
     * police.
     */
    uint16_t low = vm86_mem_read8(mem, linear);
    uint16_t high = vm86_mem_read8(mem, linear + 1);

    return (uint16_t)(low | (high << 8));
}

void vm86_mem_write16(struct vm86_mem *mem, uint32_t linear, uint16_t value)
{
    vm86_mem_write8(mem, linear, (uint8_t)(value & 0xFF));
    vm86_mem_write8(mem, linear + 1, (uint8_t)(value >> 8));
}
