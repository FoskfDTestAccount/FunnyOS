/*
 * The opcode dispatch contract.
 *
 * ---------------------------------------------------------------------
 * How the work is split
 *
 * The 8086's 256 opcodes are not grouped by what they do; they are
 * grouped by how they are encoded, and the two do not line up. ALU
 * operations appear in four unrelated places in the map, and opcode FF
 * contains increment, decrement, call, jump and push in one eight-way
 * group.
 *
 * So the split below is by encoding, each group owning a contiguous or
 * near-contiguous stretch of the map, with the awkward shares resolved by
 * one file calling a helper declared here. It is chosen so that no two
 * files touch the same opcode -- which is what makes it possible for them
 * to be written independently.
 *
 *   ops_alu.c   arithmetic, logic, shifts, tests, INC/DEC, MUL/DIV
 *   ops_mov.c   data movement, including the stack
 *   ops_str.c   the string instructions and their REP prefixes
 *   ops_ctl.c   control flow, interrupts, flag manipulation, I/O
 *   ops_186.c   the 80186 additions
 *
 * Each exports a 256-entry table, NULL wherever it is not the owner. The
 * run loop merges them and reports any opcode claimed twice -- which is
 * the failure mode of parallel work on a shared map, and is checked for
 * rather than hoped against.
 *
 * ---------------------------------------------------------------------
 * What a handler may and may not do
 *
 * A handler executes exactly one instruction and returns. It may read and
 * write registers, memory and flags; it must not fetch across an
 * instruction boundary, and it must not assume anything about what came
 * before it.
 *
 * It must not consult cpu->prefix directly for its own purposes. Prefixes
 * are for the operand addressing in decode.c and for the string
 * instructions; everything else has already had its prefix applied by the
 * time it runs.
 */
#ifndef VM86_OPS_H
#define VM86_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/decode.h>

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

enum vm86_result {
    /* The instruction completed. Continue at CS:IP, which the handler has
     * already updated if it was a jump. */
    VM86_CONTINUE = 0,

    /* HLT executed. Nothing runs until something outside resumes the
     * processor. */
    VM86_HALT,

    /* An exception was raised. cpu->fault holds the vector. The run loop
     * stops; delivering the interrupt is not this layer's job. */
    VM86_FAULT,

    /* Something on the host is broken: the opcode tables could not be
     * merged, so at least one opcode is claimed twice and no table can
     * be trusted to reach the handler it should. No instruction was
     * executed, and cpu->fault is meaningless -- nothing is wrong with
     * the guest, which is why this is not an exception and carries no
     * vector.
     *
     * It is deliberately not VM86_HALT. The three values above all
     * describe something the guest did; this one describes something
     * the host did, and conflating the two is how a broken emulator
     * gets mistaken for a working one. A caller that stops on HLT
     * reports a normal ending -- the acceptance test for the
     * integration asserts exactly that, and would pass. */
    VM86_INTERNAL_ERROR,
};

/*
 * A handler executes one instruction and returns what it did.
 *
 * `opcode` is the byte that was dispatched, after any prefixes have been
 * consumed. Handlers that need it get it for free; the rest write
 * `(void)opcode;` once and forget it.
 *
 * Passing it is worth the small noise. The 8086's map is full of ranges
 * where one function serves sixty-four opcodes that differ only in which
 * three bits say what to do -- 0x00 to 0x3F is eight operations in six
 * forms each -- and a handler that cannot see its own opcode cannot serve
 * them. The alternative is forty-eight near-identical wrappers per range,
 * which is more code and more places to be wrong.
 */
typedef enum vm86_result (*vm86_op_fn)(struct vm86_cpu *cpu, uint8_t opcode);

/*
 * Interrupt vectors this layer raises.
 *
 * A fault is reported, not delivered: the run loop stops and hands the
 * vector to whoever is running the machine. Installing handlers for them
 * is M4's job, along with the rest of the interrupt model.
 */
#define VM86_VECTOR_DIVIDE_ERROR   0   /* division by zero, or a quotient
                                        * that will not fit */
#define VM86_VECTOR_BOUND          5   /* BOUND found a value out of range */
#define VM86_VECTOR_INVALID_OPCODE 6   /* not an instruction on this part */

/*
 * Execute one instruction.
 *
 * Clears the prefix state, fetches and dispatches, and returns what the
 * handler returned. Sets cpu->insn_count.
 */
enum vm86_result vm86_step(struct vm86_cpu *cpu);

/* ------------------------------------------------------------------ */
/* The tables                                                          */
/* ------------------------------------------------------------------ */

/*
 * One per group file, 256 entries, NULL where that file is not the owner.
 * Defined by the group files themselves.
 */
extern const vm86_op_fn vm86_ops_alu[256];
extern const vm86_op_fn vm86_ops_mov[256];
extern const vm86_op_fn vm86_ops_str[256];
extern const vm86_op_fn vm86_ops_ctl[256];
extern const vm86_op_fn vm86_ops_186[256];

/*
 * Merge the groups into one table and report any opcode claimed by two of
 * them.
 *
 * Returns true when every opcode is either claimed exactly once or left
 * unclaimed. An unclaimed opcode is not an error: it means the
 * instruction does not exist on this processor, and vm86_step() raises
 * the invalid-opcode exception when it is reached. A doubly-claimed one
 * is an error, and one that would otherwise be invisible -- whichever
 * table was merged last would simply win.
 */
bool vm86_ops_build(void);

/*
 * Build the dispatch table the run loop uses out of a set of group tables.
 *
 * `groups` is `count` tables of 256 entries, and `names` names them, one
 * each. Every non-NULL slot goes into the table; the first group to claim
 * a slot keeps it, and a slot that two groups both claim is reported
 * through the conflict reporter and makes the return value false -- after
 * which vm86_step() answers VM86_INTERNAL_ERROR rather than dispatching
 * through the table it was given.
 *
 * vm86_ops_build() is this with the five groups above, which is what
 * makes a table built here the one the machine runs on.
 *
 * Exposed because the failure it describes cannot be produced from those
 * five: they do not collide. A caller that wants to see what the machine
 * does with a table that does has to supply one, and that is the only way
 * either the report or the refusal can be tested at all.
 *
 * The table stays installed until the next call -- including a call that
 * fails, which does not leave the previous table in place -- so a caller
 * that installs a broken one has to install a good one afterwards.
 */
bool vm86_ops_build_from(const vm86_op_fn *const *groups,
                         const char *const *names, size_t count);

/*
 * Called once for each opcode two groups both claim.
 *
 * A callback rather than a printf, because this code has to run in three
 * places with three different ideas of what output is: a host unit test,
 * a user-space process, and nothing at all. Letting the embedder decide
 * is cheaper than picking one and guarding it in two directions.
 */
typedef void (*vm86_conflict_fn)(void *ctx, uint8_t opcode,
                                 const char *first, const char *second);

void vm86_set_conflict_reporter(vm86_conflict_fn fn, void *ctx);

/* The merged table, for tests and for the trace dump. NULL before
 * vm86_ops_build() has run. */
const vm86_op_fn *vm86_ops_table(void);

/* ------------------------------------------------------------------ */
/* Shared arithmetic                                                   */
/* ------------------------------------------------------------------ */

/*
 * These are implemented in ops_alu.c and used by the other groups.
 *
 * They exist because the encodings split arithmetic across files that are
 * not otherwise related: opcode FF is control flow's, but two of its
 * eight sub-operations are increment and decrement, which have to set the
 * flags exactly the way FE's do.
 */

enum vm86_alu_op {
    VM86_ALU_ADD, VM86_ALU_OR,  VM86_ALU_ADC, VM86_ALU_SBB,
    VM86_ALU_AND, VM86_ALU_SUB, VM86_ALU_XOR, VM86_ALU_CMP,
};

/*
 * Compute `dst OP src`, set the flags accordingly, and return the result.
 *
 * NOTHING IS WRITTEN. A CMP computes and discards, and a handler for it
 * simply does not store the result. That is the caller's decision and is
 * kept visible here rather than hidden behind a separate function,
 * because the one thing a reader needs to know about CMP is that it looks
 * exactly like SUB and is not.
 *
 * `width` is 8 or 16 and controls which flags are affected and how the
 * operands are masked.
 */
uint16_t vm86_alu(struct vm86_cpu *cpu, enum vm86_alu_op op, uint8_t width,
                  uint16_t dst, uint16_t src);

/*
 * Increment or decrement, and set the flags.
 *
 * Not ADD or SUB with a constant of one: INC and DEC leave CF exactly as
 * they found it, which is the only thing that distinguishes them and is
 * the reason this is a separate function rather than a convenience
 * wrapper. Code that loops `inc cx / jc somewhere` depends on it.
 */
uint16_t vm86_alu_incdec(struct vm86_cpu *cpu, uint8_t width, uint16_t value,
                         bool increment);

/* ------------------------------------------------------------------ */
/* The stack                                                           */
/* ------------------------------------------------------------------ */

/*
 * Push and pop a word, through SS:SP.
 *
 * Implemented in ops_mov.c, which owns the stack instructions, and used
 * by nearly every other group: the segment push and pop encodings live in
 * the arithmetic block, CALL and RET in control flow, and PUSHA and ENTER
 * in the 186 file.
 *
 * The segment is always SS. A segment override prefix does not change it,
 * which is worth stating because the override looks like it ought to
 * apply: the stack pointer's addressing is implicit and the prefix has
 * nothing to attach to.
 *
 * SP wraps at sixteen bits, like every other register.
 */
void     vm86_push16(struct vm86_cpu *cpu, uint16_t value);
uint16_t vm86_pop16(struct vm86_cpu *cpu);

/* ------------------------------------------------------------------ */
/* The string loop                                                     */
/* ------------------------------------------------------------------ */

/*
 * One element of a repeated string instruction.
 *
 * `bits` is 8 or 16 -- bits, not bytes. `delta` is how far SI and DI
 * move, already signed from the direction flag, worked out once by the
 * loop before it starts. `source` is the DS-side segment with any
 * segment override already applied, resolved by the loop so that no body
 * has to remember to.
 *
 * The body does one element: the memory work, whatever flags the
 * instruction sets, and advancing SI and DI by delta. Which pointers
 * move is the instruction's business -- STOS moves only DI, LODS only SI
 * -- so the loop cannot do it for them. It must not read cpu->prefix,
 * and it must not touch CX.
 *
 * A body that writes memory writes through VM86_ES and must not resolve
 * that through vm86_effective_seg(): a prefix moves the read, and there
 * is no prefix on this processor that makes a string write land anywhere
 * but ES:DI.
 */
typedef void (*vm86_str_body)(struct vm86_cpu *cpu, uint8_t bits,
                              int16_t delta, enum vm86_seg source);

/*
 * Run a string instruction, with or without a REP prefix.
 *
 * Implemented in ops_str.c, which owns the string instructions, and used
 * by ops_186.c for INS and OUTS. They are the same loop: the same REP
 * prefix, the same direction flag, the same CX=0 rule, and the same
 * asymmetry where the DS side takes a segment override and the ES side
 * never does. Two implementations of that would not stay equal, and the
 * bug they would produce is a machine where `rep movsb` works and
 * `rep insb` moves a single byte -- which is the kind of thing that gets
 * blamed on the DOS program.
 *
 * `conditional` is false for MOVS, STOS, LODS, INS and OUTS, where the
 * loop runs CX times and the flags never enter into it, and true for
 * CMPS and SCAS, where F3 stops the loop the first time ZF reads clear
 * and F2 stops it the first time ZF reads set. With `conditional` false
 * an F2 therefore behaves as an F3 without a second branch, which is the
 * reading every part since the 8086 has taken.
 *
 * CX is tested before the first iteration, so a REP with CX=0 calls the
 * body zero times; it is decremented once per iteration, the last one
 * included, so a loop stopped by the flag leaves the elements it never
 * looked at still counted in CX. A program searching with REPNE SCASB
 * branches on that number afterwards, so it is part of the result rather
 * than an implementation detail.
 *
 * With no prefix at all the body is called exactly once. Always returns
 * VM86_CONTINUE: no string instruction can fault.
 */
enum vm86_result vm86_str_repeat(struct vm86_cpu *cpu, uint8_t bits,
                                 vm86_str_body body, bool conditional);

/* ------------------------------------------------------------------ */
/* Shared flag computation                                             */
/* ------------------------------------------------------------------ */

/*
 * Implemented in flags.c, used by every group.
 */

/* PF: set when the low byte of the result has an even number of set bits.
 * It reflects only the low eight bits, at every width -- which is the
 * thing that surprises people, since a 16-bit result still has an 8-bit
 * parity flag. */
bool vm86_parity(uint8_t value);

/* ZF, SF and PF from a result, at the given width. */
void vm86_flags_result(struct vm86_cpu *cpu, uint8_t width, uint16_t result);

/* The flags after a logical operation -- AND, OR, XOR, TEST: CF and OF are
 * cleared, AF is undefined and left alone, and the rest come from the
 * result. */
void vm86_flags_logic(struct vm86_cpu *cpu, uint8_t width, uint16_t result);

#endif /* VM86_OPS_H */
