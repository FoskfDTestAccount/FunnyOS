/*
 * The corpus replayer: run one assembled sample and grade the machine.
 *
 * ---------------------------------------------------------------------
 * Why this is not a test's private helper
 *
 * Everything else under dos/tests measures one instruction at a time:
 * hand the interpreter a few bytes and some registers and compare the
 * result. That misses every error that lives *between* instructions -- a
 * carry flag an `adc` inherits from the instruction before it, a REP whose
 * count is zero, a prefix that applies to the instruction after the
 * prefix rather than to the one carrying it. Those only appear when a
 * program runs, so a program is what this takes.
 *
 * The sample set is also the bridge to the machine this interpreter is
 * for. Today `dos/` runs only inside the host test process; once the
 * kernel is behind it, the same samples and the same expected states have
 * to run inside a guest as well. So the replayer is a function, not a
 * `main`, and it reaches the outside world through a callback rather than
 * through stdio: a host test passes one that prints, a guest will pass one
 * that writes to the console, and neither is the replayer's business.
 *
 * The cost of that is this file may include nothing but <stdbool.h>,
 * <stddef.h> and <stdint.h> along with the vm86/ headers. No printf, no
 * malloc, no stdio.h -- the same rule the trace formatter in dos/dbg
 * follows, for the same reason.
 *
 * ---------------------------------------------------------------------
 * The guest image convention, which is frozen
 *
 * A sample is a flat binary -- `nasm -f bin`, assembled with `org 0x100`
 * -- and it is entered the way a .COM program is:
 *
 *     loaded at linear 0x100                    CS:IP = 0x0000:0x0100
 *     DS = ES = SS = CS = 0                     SP    = 0xFFFE
 *     FLAGS = VM86_FLAG_ALWAYS_SET
 *
 * Starting at 0x100 rather than 0 costs nothing now, when there is no PSP,
 * and it is the one thing that would otherwise have to change when M5
 * adds one: a .COM program assumes the first 256 bytes belong to DOS, and
 * a corpus whose samples all moved by 0x100 when the loader arrived would
 * no longer be a corpus anyone had verified.
 *
 * FLAGS is the project's power-on value, not the 0x0002 the assignment
 * printed. On an 8086 bits 12 to 15 are not implemented and read back
 * set (VM86_FLAG_ALWAYS_SET, see cpu.h) -- which is exactly the test
 * programs of the era used to tell one chip from another. The assignment
 * gave 0x0002 and its stated reason was bit 1, so the two agree about
 * what matters and differ only in the bits it did not mention. Nothing
 * observable changes either way: the flag comparison masks the
 * always-set bits off, as it must, since they are not the guest's to
 * control.
 */
#ifndef VM86_CORPUS_REPLAY_H
#define VM86_CORPUS_REPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* The convention, as constants                                        */
/* ------------------------------------------------------------------ */

#define VM86_CORPUS_LOAD_SEGMENT  0x0000u
#define VM86_CORPUS_LOAD_OFFSET   0x0100u
#define VM86_CORPUS_STACK_TOP     0xFFFEu
#define VM86_CORPUS_INITIAL_FLAGS VM86_FLAG_ALWAYS_SET

/*
 * How many instructions a case is allowed before the replayer gives up.
 *
 * There is a limit for every case, not only for the ones that look like
 * they could loop. A sample that hangs must not hang the suite that is
 * running it -- but neither may it be reported as a wrong answer, because
 * a reader sent looking for a bug in the arithmetic will not find one.
 * Running out of budget is its own outcome (CORPUS_INSN_LIMIT) for that
 * reason.
 *
 * A case may lower it; none of the samples here needs more.
 */
#define VM86_CORPUS_INSN_LIMIT_DEFAULT 4000u

/* ------------------------------------------------------------------ */
/* What a sample declares                                              */
/* ------------------------------------------------------------------ */

/*
 * The registers a case expects to find at the end.
 *
 * Written with designated initialisers, so every field a sample does not
 * name is zero -- which is also the state it starts in, and is why a case
 * only mentions what it changed.
 */
struct corpus_state {
    uint16_t ax, bx, cx, dx, si, di, bp, sp;
    uint16_t cs, ds, es, ss;
    uint16_t ip;
    uint16_t flags;
};

/* A run of bytes the case expects to find in memory at the end, as
 * segment:offset -- the form the sample itself addresses them in. */
struct corpus_memory {
    uint16_t segment;
    uint16_t offset;
    uint8_t  length;
    uint8_t  bytes[8];
};

enum corpus_ending {
    /* The sample ends by halting, and `final` is the state to compare. */
    CORPUS_ENDS_HALTED = 0,

    /* The sample ends by raising an exception. `fault` names the vector,
     * and the registers are not compared: the state an instruction
     * leaves behind when it faults is a half-product -- DIV may have
     * touched the flags before deciding the quotient does not fit -- and
     * pinning it would pin an accident. */
    CORPUS_ENDS_FAULTED,
};

struct corpus_case {
    const char *name;

    /* The assembled program. The driver owns the bytes and keeps them
     * alive for the call; the replayer only reads them. */
    const uint8_t *image;
    uint16_t       image_size;

    enum corpus_ending ending;
    uint8_t            fault;        /* CORPUS_ENDS_FAULTED */

    /* CORPUS_ENDS_HALTED. */
    struct corpus_state final;

    /*
     * Flags the instruction set leaves undefined and the case therefore
     * does not compare.
     *
     * Which ones those are is a fact about the last instruction that
     * wrote flags -- MUL leaves everything but CF and OF undefined, a
     * shift by more than one leaves OF undefined, a logical operation
     * leaves AF undefined -- so the mask is a consequence of the sample
     * and cannot be copied from its neighbours. Too wide a mask is the
     * dangerous direction: it hides the differences it was meant to
     * catch, and it looks exactly like a passing test.
     */
    uint16_t flags_ignore;

    const struct corpus_memory *memory;
    uint8_t                     memory_count;

    uint32_t insn_limit;             /* 0 means the default */
};

/* ------------------------------------------------------------------ */
/* What the replayer says                                              */
/* ------------------------------------------------------------------ */

/*
 * One line of difference, with no newline and no case name: the replayer
 * knows what differs but not how the embedder labels its output, and the
 * embedder is the one that knows what a line of its output should look
 * like. The host suite prefixes the sample's name; a guest would put it
 * wherever it puts text.
 */
typedef void (*vm86_corpus_report)(void *ctx, const char *text, size_t length);

enum corpus_outcome {
    /* The machine stopped where the case said, with the state it said. */
    CORPUS_PASS = 0,

    /* It stopped, but a register, a flag or a byte of memory differs. */
    CORPUS_MISMATCH,

    /* It halted where the case expected a fault, or faulted where the
     * case expected a halt. */
    CORPUS_WRONG_ENDING,

    /* It faulted, but with a different vector than the case named. */
    CORPUS_WRONG_FAULT,

    /*
     * It never stopped.
     *
     * Deliberately not folded into CORPUS_MISMATCH. A sample that loops
     * forever and a sample that computes the wrong answer are different
     * faults with different causes, and a report that calls both of them
     * "the state did not match" sends the reader after arithmetic that
     * is not there.
     */
    CORPUS_INSN_LIMIT,

    /*
     * The emulator failed and the sample did not.
     *
     * The opcode table did not merge, so nothing in the run reached the
     * handler it should have and this case has not been judged at all.
     * Kept apart from the four above for the reason the four are kept
     * apart from each other: those are verdicts on the sample, and a
     * report that delivered this one as a verdict would send the reader
     * through a sample that is fine.
     */
    CORPUS_INTERNAL_ERROR,
};

/*
 * Load `c` into `mem`, run it, and grade what is left.
 *
 * `mem` is cleared first: a case describes a machine at power-on, and a
 * machine at power-on has no memory. The caller provides the region with
 * vm86_mem_attach() and keeps it for as long as the call.
 *
 * Every difference is handed to `report`; the return value says which
 * kind of difference it was, so a driver can count and summarise without
 * re-reading its own text. `report` may be NULL when the caller only
 * wants the outcome, but then it learns nothing about what was wrong.
 */
enum corpus_outcome vm86_corpus_replay(struct vm86_mem *mem,
                                       const struct corpus_case *c,
                                       vm86_corpus_report report, void *ctx);

#endif /* VM86_CORPUS_REPLAY_H */
