/*
 * The 8086 CPU state.
 *
 * ---------------------------------------------------------------------
 * Why this header is frozen
 *
 * Every opcode handler in this directory reads and writes this structure,
 * and the handlers are being written independently of each other. So the
 * layout and the accessors here are the contract: changing them after
 * work has started means changing every file at once, which is exactly
 * the thing the split was meant to avoid.
 *
 * Add to it if something is genuinely missing. Do not reorganise it.
 *
 * ---------------------------------------------------------------------
 * Design notes
 *
 * 8-bit register views are unions rather than shifts. `al` and `ah` are
 * two halves of one 16-bit register on the real chip, and DOS programs
 * rely on that: writing AL changes AX. Spelling it as a union costs
 * nothing and removes a whole class of "I forgot to merge the halves"
 * bugs. This assumes a little-endian host, which is a real dependency and
 * is stated rather than hidden -- x86 is little-endian throughout, and
 * this emulator only ever runs on x86-64.
 *
 * Segment base addresses are cached. Without the cache every memory
 * access would shift a segment register by four and add an offset, and
 * DOS programs do that millions of times a second. The cache is
 * invalidated by a dirty bitmap rather than rebuilt eagerly, because most
 * programs load a segment once and then execute thousands of instructions
 * inside it.
 */
#ifndef VM86_CPU_H
#define VM86_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* Segment registers                                                   */
/* ------------------------------------------------------------------ */

/*
 * Segment register indices, used to address seg_base[] and the segment
 * prefix table. The order is the one the instruction encodings use, not
 * an arbitrary choice: the ModRM r/m encoding maps 0=ES, 1=CS, 2=SS,
 * 3=DS, 4=ES, 5=CS, 6=SS, 7=DS.
 */
enum vm86_seg {
    VM86_ES = 0,
    VM86_CS = 1,
    VM86_SS = 2,
    VM86_DS = 3,
    VM86_SEG_COUNT = 4,
};

#define VM86_NO_SEGMENT  (-1)   /* no segment override in effect */

/*
 * No exception was raised.
 *
 * The fault field below cannot use zero for this, because zero is already
 * taken: divide error is vector 0, and the invalid opcode trap is 6. A
 * field where zero meant both "nothing happened" and "the program divided
 * by zero" could not answer the one question it exists to answer, and the
 * only reason that never broke anything is that every caller also looked
 * at what vm86_step returned.
 *
 * Every vector this layer can raise is a real one, so the sentinel sits
 * outside the range.
 */
#define VM86_NO_FAULT   0xFFu

/* ------------------------------------------------------------------ */
/* FLAGS                                                               */
/* ------------------------------------------------------------------ */

/*
 * Flag bits, in their architectural positions.
 *
 * The names are the ones the Intel manuals use; DOS programs and the
 * manuals for them use the same ones, so there is no reason to invent
 * anything.
 */
#define VM86_CF  0x0001u   /* carry */
#define VM86_PF  0x0004u   /* parity of the low byte */
#define VM86_AF  0x0010u   /* auxiliary carry, out of bit 3 */
#define VM86_ZF  0x0040u   /* zero */
#define VM86_SF  0x0080u   /* sign */
#define VM86_TF  0x0100u   /* trap: single-step */
#define VM86_IF  0x0200u   /* interrupt enable */
#define VM86_DF  0x0400u   /* direction, for the string instructions */
#define VM86_OF  0x0800u   /* overflow */

/* The bits a program can change. Everything else in the register reads
 * as a constant and is handled separately. */
#define VM86_FLAG_MASK 0x0FD5u

/*
 * Bits that always read as one.
 *
 * Bit 1 is set on every x86 ever made. Bits 12-15 are 1 on the 8086 and
 * 8088, which is the chip this emulator claims to be: on those parts the
 * bits are not implemented and read back set.
 *
 * This is a real compatibility decision rather than a detail. Programs of
 * that era detect what they are running on by pushing FLAGS and looking
 * at those bits -- "if 0xF000 comes back, this is an 8086" -- and a
 * program that concludes it is on a 386 may take a code path this
 * emulator cannot run. Later CPUs read those bits as zero, and if the
 * emulator ever targets one, this constant is the single line to change.
 */
#define VM86_FLAG_ALWAYS_SET 0xF002u

/* ------------------------------------------------------------------ */
/* Prefix state                                                        */
/* ------------------------------------------------------------------ */

/*
 * Prefixes seen for the instruction currently being executed.
 *
 * They are transient: cleared before each instruction is fetched, set by
 * the prefix handlers, and consulted by whatever follows. That is how the
 * hardware does it -- a prefix modifies the next instruction and is
 * forgotten -- and it is why they live here rather than being passed
 * through the dispatch as arguments.
 */
struct vm86_prefix {
    int8_t  segment;   /* a VM86_SEG_* value, or VM86_NO_SEGMENT */
    uint8_t repeat;    /* 0, 0xF3 (REP/REPE) or 0xF2 (REPNE) */
    bool    lock;      /* F0. Accepted and ignored: there is one CPU. */
};

/* ------------------------------------------------------------------ */
/* The CPU                                                             */
/* ------------------------------------------------------------------ */

struct vm86_cpu {
    /* --- General purpose registers, with 8-bit views --- */
    union { uint16_t ax; struct { uint8_t al, ah; }; };
    union { uint16_t bx; struct { uint8_t bl, bh; }; };
    union { uint16_t cx; struct { uint8_t cl, ch; }; };
    union { uint16_t dx; struct { uint8_t dl, dh; }; };
    uint16_t si, di, bp, sp;

    /* --- Segment registers and the instruction pointer --- */
    uint16_t cs, ds, es, ss;
    uint16_t ip;
    uint16_t flags;

    /*
     * Segment base addresses: seg_base[s] == seg[s] << 4.
     *
     * Rebuilt lazily. A segment register write clears the matching bit in
     * seg_dirty, and the accessors in decode.h or mem.c refresh whichever
     * bases are marked before using them. Handlers must never write
     * seg_base[] directly -- go through vm86_set_seg().
     */
    uint32_t seg_base[VM86_SEG_COUNT];
    uint8_t  seg_dirty;

    /* --- Machine --- */
    struct vm86_mem *mem;

    /*
     * The exception raised by the instruction just executed, as an
     * interrupt vector, or VM86_NO_FAULT.
     *
     * A handler that raises one sets this and returns VM86_FAULT; the run
     * loop stops and the caller decides what to do about it. Divide error
     * is vector 0 and the invalid opcode trap is vector 6.
     *
     * Cleared at the start of every instruction, so what is here always
     * describes the step just taken rather than one from earlier in the
     * run. Before that clearing existed, a fault left its vector behind
     * for every later instruction to read -- invisible in practice only
     * because nothing read the field without checking the result first.
     */
    uint8_t fault;

    /* Set by HLT, cleared by anything that resumes the processor. */
    bool halted;

    /* Prefixes for the instruction being executed. Cleared per
     * instruction by the run loop. */
    struct vm86_prefix prefix;

    /* Instructions executed. Diagnostics and trace limits only; no
     * handler may branch on it. */
    uint64_t insn_count;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Reset to the state a machine comes up in: segment registers zero, IP
 * zero, FLAGS holding only the always-set bits. `mem` may be NULL and
 * set later with vm86_set_memory(). */
void vm86_reset(struct vm86_cpu *cpu, struct vm86_mem *mem);

void vm86_set_memory(struct vm86_cpu *cpu, struct vm86_mem *mem);

/* ------------------------------------------------------------------ */
/* Segments                                                            */
/* ------------------------------------------------------------------ */

/* Write a segment register and invalidate its cached base. */
void vm86_set_seg(struct vm86_cpu *cpu, enum vm86_seg which, uint16_t value);

/* Read a segment register. */
uint16_t vm86_get_seg(const struct vm86_cpu *cpu, enum vm86_seg which);

/* Make every cached base valid. The accessors do this on demand, but a
 * caller about to run a long stretch of code can pay for it once. */
void vm86_flush_segments(struct vm86_cpu *cpu);

/* ------------------------------------------------------------------ */
/* Flags, at run time                                                  */
/* ------------------------------------------------------------------ */

/*
 * Set or clear one flag.
 *
 * Every handler needs these, and they are here rather than in ops.h
 * because they are about the register, not about the instruction set.
 */
static inline void vm86_flag_set(struct vm86_cpu *cpu, uint16_t flag, bool on)
{
    if (on)
        cpu->flags |= flag;
    else
        cpu->flags &= (uint16_t)~flag;
}

static inline bool vm86_flag_test(const struct vm86_cpu *cpu, uint16_t flag)
{
    return (cpu->flags & flag) != 0;
}

/*
 * Force the bits that always read as one.
 *
 * Required after anything that assigns to the register wholesale -- POPF,
 * IRET, or a guest program writing FLAGS through a debugger. Without it,
 * a program that pushes FLAGS and inspects the result sees whatever the
 * guest last put there, which on an 8086 is not a thing that can happen.
 */
static inline void vm86_flags_normalise(struct vm86_cpu *cpu)
{
    cpu->flags = (uint16_t)((cpu->flags & VM86_FLAG_MASK) | VM86_FLAG_ALWAYS_SET);
}

#endif /* VM86_CPU_H */
