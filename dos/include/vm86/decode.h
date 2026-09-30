/*
 * Instruction decoding: registers, operands, effective addresses.
 *
 * ---------------------------------------------------------------------
 * Why this is one file and not five
 *
 * Every opcode handler needs three things: to read a register by its
 * encoding number, to fetch the next byte of the instruction, and to work
 * out what the ModRM byte points at. Those three things have to agree
 * with each other exactly -- the ModRM byte's meaning depends on which
 * registers the instruction uses, and the displacement it implies has to
 * be consumed before any immediate operand can be read.
 *
 * So they live together, are written once, and are used by everything.
 * The opcode handlers are separate; this is shared.
 *
 * ---------------------------------------------------------------------
 * The register encoding, and the trap in it
 *
 * THE 8-BIT AND 16-BIT REGISTER ORDERS ARE DIFFERENT.
 *
 *    16-bit, ModRM reg or r/m = 0..7:  AX CX DX BX SP BP SI DI
 *     8-bit, ModRM reg or r/m = 0..7:  AL CL DL BL AH CH DH BH
 *
 * The low four agree. The high four do not: index 4 is SP as a word and
 * AH as a byte, and 5 is BP versus CH. Getting this wrong produces an
 * emulator that is correct for every instruction using AX, BX, CX or DX
 * and wrong for everything else, which is a genuinely confusing bug to
 * chase.
 *
 * vm86_reg_get8() and vm86_reg_get16() below apply the right table. Do
 * not index the register file by hand.
 */
#ifndef VM86_DECODE_H
#define VM86_DECODE_H

#include <stdint.h>

#include <vm86/cpu.h>

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

/* 16-bit register numbers as they appear in encodings. */
enum vm86_reg16 {
    VM86_REG_AX = 0, VM86_REG_CX, VM86_REG_DX, VM86_REG_BX,
    VM86_REG_SP,     VM86_REG_BP, VM86_REG_SI, VM86_REG_DI,
};

/* 8-bit register numbers as they appear in encodings. Note that these are
 * NOT the same order as the 16-bit ones. See the note above. */
enum vm86_reg8 {
    VM86_REG_AL = 0, VM86_REG_CL, VM86_REG_DL, VM86_REG_BL,
    VM86_REG_AH,     VM86_REG_CH, VM86_REG_DH, VM86_REG_BH,
};

uint16_t vm86_reg_get16(struct vm86_cpu *cpu, uint8_t index);
uint8_t  vm86_reg_get8 (struct vm86_cpu *cpu, uint8_t index);
void     vm86_reg_set16(struct vm86_cpu *cpu, uint8_t index, uint16_t value);
void     vm86_reg_set8 (struct vm86_cpu *cpu, uint8_t index, uint8_t value);

/* ------------------------------------------------------------------ */
/* The instruction stream                                              */
/* ------------------------------------------------------------------ */

/*
 * Read the next byte or word at CS:IP and advance IP past it.
 *
 * IP wraps within the segment, as it does on the hardware: fetching past
 * FFFF continues at 0000 rather than running into the next segment. That
 * is not a corner case worth skipping -- a program whose code fills a
 * segment relies on it.
 *
 * Order matters when reading an instruction. The ModRM byte comes first,
 * then any displacement it implies, then the immediate. A handler that
 * fetches its immediate before decoding ModRM reads the wrong bytes, and
 * will do so silently on every instruction where the displacement is
 * non-zero.
 */
uint8_t  vm86_fetch8(struct vm86_cpu *cpu);
uint16_t vm86_fetch16(struct vm86_cpu *cpu);

/* The linear address of CS:IP, for callers that need to see where they
 * are -- a trace, or an error message worth reading. */
uint32_t vm86_code_address(struct vm86_cpu *cpu);

/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

/* The segment a reference uses, after the segment override prefix. Pass
 * the segment the instruction would use anyway; the prefix wins if one is
 * in effect, and is then forgotten. */
enum vm86_seg vm86_effective_seg(struct vm86_cpu *cpu, enum vm86_seg natural);

/*
 * The linear address of seg:offset.
 *
 * Segment base plus offset, with neither wrapped. The A20 fold and the
 * bounds check happen at the memory access itself, in mem.c, so that
 * every path through the machine applies them exactly once and in the
 * same place. A caller that wants the raw address -- a trace, or an error
 * message -- gets it.
 */
uint32_t vm86_linear(struct vm86_cpu *cpu, enum vm86_seg seg, uint16_t offset);

/* ------------------------------------------------------------------ */
/* Operands                                                            */
/* ------------------------------------------------------------------ */

enum vm86_operand_kind {
    VM86_OPERAND_REGISTER,
    VM86_OPERAND_MEMORY,
    VM86_OPERAND_IMMEDIATE,
};

/*
 * Where an operand is and how wide -- not what it holds.
 *
 * Reading and writing are separate calls, so that an instruction can read
 * both of its operands before writing either. Most instructions need
 * that, and the ones that do not are not the ones that go wrong.
 *
 * An operand resolved from ModRM is stable: it names a register or a
 * linear address, and neither changes between the read and the write.
 * That is why this can be passed around rather than re-decoded.
 */
struct vm86_operand {
    enum vm86_operand_kind kind;
    uint8_t  width;    /* 8 or 16 */
    uint8_t  reg;      /* VM86_OPERAND_REGISTER: a VM86_REG_* number */
    uint32_t addr;     /* VM86_OPERAND_MEMORY: a linear address */
    uint16_t value;    /* VM86_OPERAND_IMMEDIATE */

    /*
     * Which segment a memory operand was formed through, so that a string
     * instruction or a debugger can tell. Not used by the accessors,
     * which work on the resolved linear address.
     */
    enum vm86_seg seg;
};

/* Read and write through a resolved operand.
 *
 * vm86_operand_write() on an immediate does nothing and reports so. It is
 * a programming error rather than a guest-visible condition: an
 * instruction that tries to store to an immediate is one whose handler
 * took the wrong branch.
 */
uint16_t vm86_operand_read (struct vm86_cpu *cpu, const struct vm86_operand *op);
void     vm86_operand_write(struct vm86_cpu *cpu, const struct vm86_operand *op,
                            uint16_t value);

/* ------------------------------------------------------------------ */
/* ModRM                                                               */
/* ------------------------------------------------------------------ */

struct vm86_modrm {
    uint8_t mod;
    uint8_t reg;    /* also the opcode extension for the group encodings */
    uint8_t rm;

    /* The r/m operand, already resolved. */
    struct vm86_operand operand;
};

/*
 * Decode the ModRM byte at CS:IP, resolving its r/m operand.
 *
 * `width` is 8 or 16 and settles one question the byte alone cannot
 * answer: when mod == 3 the r/m field names a register, and which
 * register depends on how wide the operation is.
 *
 * On return, IP has advanced past the ModRM byte and past any
 * displacement. An immediate operand, if the instruction has one, is the
 * next thing in the stream.
 */
void vm86_fetch_modrm(struct vm86_cpu *cpu, struct vm86_modrm *out, uint8_t width);

/*
 * Sign-extend an 8-bit displacement or immediate to 16 bits.
 *
 * Used by the 0x83 form of the arithmetic group, which encodes an
 * immediate byte that means a 16-bit signed value. Writing it as a cast
 * gets it wrong for every negative value, which is a mistake that passes
 * every test with positive numbers.
 */
static inline uint16_t vm86_sign_extend8(uint8_t value)
{
    return (uint16_t)(int16_t)(int8_t)value;
}

#endif /* VM86_DECODE_H */
