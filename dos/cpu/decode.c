#include <vm86/decode.h>

#include <vm86/ops.h>

/*
 * Register access.
 *
 * A switch rather than an indexed array, because the register file is not
 * laid out in encoding order and rearranging it to be would make the
 * structure unreadable for the sake of a jump table. The compiler turns
 * each of these into a table of addresses anyway, and the branch predictor
 * learns it within a few iterations.
 *
 * The one thing to be careful of is the 8-bit order, which is not the
 * 16-bit order. See the note at the top of decode.h.
 */

uint16_t vm86_reg_get16(struct vm86_cpu *cpu, uint8_t index)
{
    switch (index & 7) {
    case VM86_REG_AX: return cpu->ax;
    case VM86_REG_CX: return cpu->cx;
    case VM86_REG_DX: return cpu->dx;
    case VM86_REG_BX: return cpu->bx;
    case VM86_REG_SP: return cpu->sp;
    case VM86_REG_BP: return cpu->bp;
    case VM86_REG_SI: return cpu->si;
    case VM86_REG_DI: return cpu->di;
    default:          return 0;
    }
}

void vm86_reg_set16(struct vm86_cpu *cpu, uint8_t index, uint16_t value)
{
    switch (index & 7) {
    case VM86_REG_AX: cpu->ax = value; break;
    case VM86_REG_CX: cpu->cx = value; break;
    case VM86_REG_DX: cpu->dx = value; break;
    case VM86_REG_BX: cpu->bx = value; break;
    case VM86_REG_SP: cpu->sp = value; break;
    case VM86_REG_BP: cpu->bp = value; break;
    case VM86_REG_SI: cpu->si = value; break;
    case VM86_REG_DI: cpu->di = value; break;
    default:          break;
    }
}

uint8_t vm86_reg_get8(struct vm86_cpu *cpu, uint8_t index)
{
    switch (index & 7) {
    case VM86_REG_AL: return cpu->al;
    case VM86_REG_CL: return cpu->cl;
    case VM86_REG_DL: return cpu->dl;
    case VM86_REG_BL: return cpu->bl;
    case VM86_REG_AH: return cpu->ah;
    case VM86_REG_CH: return cpu->ch;
    case VM86_REG_DH: return cpu->dh;
    case VM86_REG_BH: return cpu->bh;
    default:          return 0;
    }
}

void vm86_reg_set8(struct vm86_cpu *cpu, uint8_t index, uint8_t value)
{
    switch (index & 7) {
    case VM86_REG_AL: cpu->al = value; break;
    case VM86_REG_CL: cpu->cl = value; break;
    case VM86_REG_DL: cpu->dl = value; break;
    case VM86_REG_BL: cpu->bl = value; break;
    case VM86_REG_AH: cpu->ah = value; break;
    case VM86_REG_CH: cpu->ch = value; break;
    case VM86_REG_DH: cpu->dh = value; break;
    case VM86_REG_BH: cpu->bh = value; break;
    default:          break;
    }
}

/* ------------------------------------------------------------------ */
/* The instruction stream                                              */
/* ------------------------------------------------------------------ */

uint8_t vm86_fetch8(struct vm86_cpu *cpu)
{
    if (cpu->seg_dirty)
        vm86_flush_segments(cpu);

    uint32_t addr = cpu->seg_base[VM86_CS] + cpu->ip;

    /* IP wraps inside its segment. A program whose code fills the segment
     * depends on this, so it is not a corner case to skip. */
    cpu->ip++;

    return vm86_mem_read8(cpu->mem, addr);
}

uint16_t vm86_fetch16(struct vm86_cpu *cpu)
{
    uint16_t low  = vm86_fetch8(cpu);
    uint16_t high = vm86_fetch8(cpu);

    return (uint16_t)(low | (high << 8));
}

uint32_t vm86_code_address(struct vm86_cpu *cpu)
{
    return vm86_linear(cpu, VM86_CS, cpu->ip);
}

/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

enum vm86_seg vm86_effective_seg(struct vm86_cpu *cpu, enum vm86_seg natural)
{
    if (cpu->prefix.segment != VM86_NO_SEGMENT)
        return (enum vm86_seg)cpu->prefix.segment;

    return natural;
}

uint32_t vm86_linear(struct vm86_cpu *cpu, enum vm86_seg seg, uint16_t offset)
{
    if (cpu->seg_dirty)
        vm86_flush_segments(cpu);

    return cpu->seg_base[seg] + offset;
}

/* ------------------------------------------------------------------ */
/* Operands                                                            */
/* ------------------------------------------------------------------ */

uint16_t vm86_operand_read(struct vm86_cpu *cpu, const struct vm86_operand *op)
{
    switch (op->kind) {
    case VM86_OPERAND_REGISTER:
        return op->width == 8 ? vm86_reg_get8(cpu, op->reg)
                              : vm86_reg_get16(cpu, op->reg);

    case VM86_OPERAND_MEMORY:
        return op->width == 8 ? vm86_mem_read8(cpu->mem, op->addr)
                              : vm86_mem_read16(cpu->mem, op->addr);

    case VM86_OPERAND_IMMEDIATE:
        return op->value;
    }

    return 0;
}

void vm86_operand_write(struct vm86_cpu *cpu, const struct vm86_operand *op,
                        uint16_t value)
{
    switch (op->kind) {
    case VM86_OPERAND_REGISTER:
        if (op->width == 8)
            vm86_reg_set8(cpu, op->reg, (uint8_t)value);
        else
            vm86_reg_set16(cpu, op->reg, value);
        break;

    case VM86_OPERAND_MEMORY:
        if (op->width == 8)
            vm86_mem_write8(cpu->mem, op->addr, (uint8_t)value);
        else
            vm86_mem_write16(cpu->mem, op->addr, value);
        break;

    case VM86_OPERAND_IMMEDIATE:
        /*
         * Not a guest-visible condition: an instruction that stores to an
         * immediate is one whose handler took a branch it should not
         * have. Doing nothing is the safest response and leaves the
         * mistake visible as a wrong result rather than as corruption
         * somewhere else.
         */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ModRM                                                               */
/* ------------------------------------------------------------------ */

void vm86_fetch_modrm(struct vm86_cpu *cpu, struct vm86_modrm *out, uint8_t width)
{
    uint8_t byte = vm86_fetch8(cpu);

    out->mod = (uint8_t)((byte >> 6) & 3);
    out->reg = (uint8_t)((byte >> 3) & 7);
    out->rm  = (uint8_t)(byte & 7);

    out->operand.width = width;
    out->operand.reg   = 0;
    out->operand.addr  = 0;
    out->operand.value = 0;
    out->operand.seg   = VM86_NO_SEGMENT;

    /* mod == 3 means the r/m field names a register, not memory. Which
     * register is what `width` settles: the byte alone cannot say whether
     * index 4 means SP or AH. */
    if (out->mod == 3) {
        out->operand.kind = VM86_OPERAND_REGISTER;
        out->operand.reg  = out->rm;
        return;
    }

    uint16_t offset  = 0;
    bool     uses_bp = false;

    if (out->mod == 0 && out->rm == 6) {
        /*
         * The one encoding that is not a base-plus-index form: a direct
         * sixteen-bit address. It reads its displacement from the
         * instruction, like the other mods, but has no base register to
         * add it to.
         */
        offset = vm86_fetch16(cpu);
    } else {
        switch (out->rm) {
        case 0: offset = (uint16_t)(cpu->bx + cpu->si); break;
        case 1: offset = (uint16_t)(cpu->bx + cpu->di); break;
        case 2: offset = (uint16_t)(cpu->bp + cpu->si); uses_bp = true; break;
        case 3: offset = (uint16_t)(cpu->bp + cpu->di); uses_bp = true; break;
        case 4: offset = cpu->si; break;
        case 5: offset = cpu->di; break;
        case 6: offset = cpu->bp; uses_bp = true; break;
        case 7: offset = cpu->bx; break;
        default: break;
        }

        /*
         * The offset arithmetic wraps at sixteen bits, which is why every
         * step above casts back down. A guest writing to [BP+DI] past the
         * end of a segment wraps within it; letting the host's 32-bit
         * arithmetic carry would silently change which byte is written.
         */
        if (out->mod == 1)
            offset = (uint16_t)(offset + vm86_sign_extend8(vm86_fetch8(cpu)));
        else if (out->mod == 2)
            offset = (uint16_t)(offset + vm86_fetch16(cpu));
    }

    /*
     * Addressing through BP defaults to the stack segment; everything
     * else defaults to the data segment. That asymmetry is not arbitrary
     * -- it is what makes BP usable as a frame pointer without the
     * compiler having to say which segment each access uses.
     */
    enum vm86_seg seg = vm86_effective_seg(cpu, uses_bp ? VM86_SS : VM86_DS);

    out->operand.kind = VM86_OPERAND_MEMORY;
    out->operand.addr = vm86_linear(cpu, seg, offset);
    out->operand.seg  = seg;
}
