#include <vm86/cpu.h>

#include <libk/string.h>

#define VM86_SEG_DIRTY_ALL ((uint8_t)((1u << VM86_SEG_COUNT) - 1u))

void vm86_reset(struct vm86_cpu *cpu, struct vm86_mem *mem)
{
    memset(cpu, 0, sizeof(*cpu));

    cpu->mem = mem;

    /*
     * A real machine comes up with FLAGS holding only the bits that are
     * hardwired. Everything else -- including IF -- is clear, which is
     * why a DOS program that wants interrupts has to enable them rather
     * than finding them already on.
     */
    cpu->flags = VM86_FLAG_ALWAYS_SET;

    cpu->prefix.segment = VM86_NO_SEGMENT;

    /* No cached segment bases are valid yet. */
    cpu->seg_dirty = VM86_SEG_DIRTY_ALL;
}

void vm86_set_memory(struct vm86_cpu *cpu, struct vm86_mem *mem)
{
    cpu->mem = mem;
}

uint16_t vm86_get_seg(const struct vm86_cpu *cpu, enum vm86_seg which)
{
    switch (which) {
    case VM86_ES: return cpu->es;
    case VM86_CS: return cpu->cs;
    case VM86_SS: return cpu->ss;
    case VM86_DS: return cpu->ds;
    default:      return 0;
    }
}

void vm86_set_seg(struct vm86_cpu *cpu, enum vm86_seg which, uint16_t value)
{
    switch (which) {
    case VM86_ES: cpu->es = value; break;
    case VM86_CS: cpu->cs = value; break;
    case VM86_SS: cpu->ss = value; break;
    case VM86_DS: cpu->ds = value; break;
    default:      return;
    }

    /*
     * Marked rather than recomputed.
     *
     * A program typically loads a segment once and then runs thousands of
     * instructions inside it, so rebuilding eagerly would do the same
     * shift four times for every one time it changed. The shift itself is
     * trivial; not doing it at all is better.
     */
    cpu->seg_dirty |= (uint8_t)(1u << which);
}

void vm86_flush_segments(struct vm86_cpu *cpu)
{
    for (int i = 0; i < VM86_SEG_COUNT; i++) {
        if (!(cpu->seg_dirty & (1u << i)))
            continue;

        /*
         * A segment register holds a paragraph number: the base is the
         * value shifted left by four. That is the whole of the 8086's
         * addressing model, and the reason a segment can only begin on a
         * sixteen-byte boundary.
         */
        cpu->seg_base[i] = (uint32_t)vm86_get_seg(cpu, (enum vm86_seg)i) << 4;
    }

    cpu->seg_dirty = 0;
}
