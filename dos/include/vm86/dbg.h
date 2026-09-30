/*
 * The single-step debugger.
 *
 * ---------------------------------------------------------------------
 * What this is for
 *
 * M3's acceptance criterion is that the interpreter executes hand-written
 * binaries "with the registers and flags in the right state", and a state
 * nobody can see is a state nobody can claim. The risk register says the
 * same thing from the other end: with no shell and no filesystem worth
 * the name, debugging a DOS program is painful, so the tools have to be
 * here before the programs are.
 *
 * So: step exactly one instruction, show the machine, show memory, and
 * keep a record of what the step did.
 *
 * ---------------------------------------------------------------------
 * The one interface that has to be frozen: output is a callback
 *
 * The same code runs in two places that disagree about what output is --
 * a host unit test, where it is printf, and a FunnyOS process, where it
 * is SYS_WRITE -- so nothing here returns a string, nothing here calls
 * printf, and nothing here reads or writes any global output state. Every
 * formatter takes a sink and hands it finished pieces of text.
 *
 * Each formatter builds one line in a small array on the stack and hands
 * the line over complete. There is no allocation to do and nothing to
 * free, which is what lets this file link into the freestanding build
 * alongside the interpreter.
 *
 * ---------------------------------------------------------------------
 * The trap: the length of an instruction cannot be measured, only decoded
 *
 * The obvious way to record the bytes of an instruction is to step first
 * and see how far IP moved. It is wrong, and it is wrong exactly when a
 * debugger is most needed. IP does not have to move forward: `jmp $`
 * leaves it where it was, a backward jump moves it backwards, and an
 * instruction that raises an exception may be reported without the
 * machine ever reaching the next byte. Subtracting before from after
 * gives zero, or a negative, or -- taken as unsigned -- most of 64K.
 *
 * So the length is decoded before the step, from the bytes at CS:IP, read
 * with vm86_mem_read8() and nothing else: reading the instruction stream
 * through the fetch helpers would advance IP, and reading it late is
 * impossible because by then the pointer has already moved.
 *
 * The record keeps both numbers, and vm86_dbg_step() reports whether they
 * agreed, so that a disagreement arrives as a fact about the trace -- "IP
 * did not move by the decoded length" -- rather than as an unexplainable
 * dump. It is also the only check that would catch a length decoder that
 * has quietly stopped agreeing with the emulator.
 *
 * ---------------------------------------------------------------------
 * What is not here, deliberately: a disassembler
 *
 * Turning bytes into `add ax, bx` is a project of its own, and half of
 * one is worse than none: it prints a confident wrong answer and the
 * reader believes it. What a trace prints instead is the raw bytes and
 * the length -- `0100: 01 D8 (2)` -- which any external disassembler will
 * turn back into the right mnemonic, and which cannot lie.
 *
 * That decision has an obligation attached to it: the bytes have to
 * actually be recorded. A trace that kept only CS:IP would turn "there is
 * no disassembler" from a delegation into a hole, because there would be
 * nothing for the external tool to read.
 */
#ifndef VM86_DBG_H
#define VM86_DBG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vm86/cpu.h>
#include <vm86/ops.h>

/* ------------------------------------------------------------------ */
/* The output sink                                                     */
/* ------------------------------------------------------------------ */

/*
 * Somewhere to put text.
 *
 * `text` is not required to be NUL-terminated: `length` is the whole
 * truth, and a caller that wants a C string copies the bytes to where it
 * keeps one. That is deliberate -- the host build appends these to a
 * buffer and the kernel build hands them to SYS_WRITE, and neither wants
 * the formatter to decide which.
 */
typedef void (*vm86_dbg_write)(void *ctx, const char *text, size_t length);

struct vm86_dbg_out {
    vm86_dbg_write write;
    void          *ctx;
};

/* ------------------------------------------------------------------ */
/* Decoding one instruction's length                                   */
/* ------------------------------------------------------------------ */

/*
 * The longest byte string a trace will keep.
 *
 * The longest thing this processor can actually execute is two prefix
 * bytes, an opcode, a ModRM byte, a sixteen-bit displacement and a
 * sixteen-bit immediate: eight bytes. Sixteen is room to spare and a
 * whole number of hex columns, which is why it is sixteen and not eight.
 */
#define VM86_DBG_MAX_BYTES 16

/*
 * How many prefix bytes the scan will count before giving up.
 *
 * The run loop takes prefixes until it finds something that is not one,
 * so in principle a stream full of them is an instruction of unbounded
 * length. Real code has at most two. Beyond this the scan stops, which
 * bounds the work a corrupted or hostile stream can cause.
 */
#define VM86_DBG_MAX_PREFIX 8

struct vm86_dbg_decode {
    uint8_t length;      /* the whole instruction, prefixes included */
    uint8_t prefixes;    /* how many of those bytes were prefixes */
    bool    transfers_control;
};

/*
 * Work out how long the instruction at CS:IP is, without running it.
 *
 * `transfers_control` is true for the instructions that send IP somewhere
 * other than the next byte: the jumps, the calls and returns, the
 * interrupts, and the loop instructions. It is the caller's answer to "is
 * it legitimate for IP not to have moved by `length`".
 *
 * An opcode no group claims is not an instruction on this machine, and
 * the run loop fetches it and raises the invalid-opcode exception without
 * looking at the byte after it, so its length is one.
 *
 * Nothing about `cpu` is modified.
 */
void vm86_dbg_decode(struct vm86_cpu *cpu, struct vm86_dbg_decode *out);

/* ------------------------------------------------------------------ */
/* The trace record                                                    */
/* ------------------------------------------------------------------ */

/*
 * One instruction, before and after.
 *
 * `after` is the whole machine, not a summary of it, because the point of
 * the record is to be able to say what actually happened -- including
 * after an instruction that faulted halfway, where the state is whatever
 * the handler had already done rather than what a completed instruction
 * would have produced.
 */
struct vm86_dbg_trace {
    /* Where the instruction was. */
    uint16_t cs, ip;

    /* The bytes, as they were before anything ran. `shown` of them are
     * valid; `length` is the real length, which can be larger when a
     * stream carries more prefixes than the record keeps bytes for. */
    uint8_t bytes[VM86_DBG_MAX_BYTES];
    uint8_t shown;
    uint8_t length;
    uint8_t prefixes;

    bool transfers_control;

    /* Whether IP moved by exactly `length`, or the instruction was one
     * that is allowed not to. False means the decoder and the emulator
     * disagree, which is a bug in one of them. */
    bool ip_advance_ok;

    /* What the step returned, and the vector when it faulted. */
    enum vm86_result result;
    uint8_t          fault;

    /* The machine the instruction left behind. */
    struct vm86_cpu after;
};

/*
 * Execute exactly one instruction, and record it.
 *
 * Equivalent to vm86_step(cpu) with a record kept of it, and returns what
 * vm86_step() returned. `trace` may be NULL, which is how a caller that
 * wants to step without recording asks for it.
 */
enum vm86_result vm86_dbg_step(struct vm86_cpu *cpu, struct vm86_dbg_trace *trace);

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

/*
 * The eight general registers, the four segment registers with their
 * cached bases, IP and FLAGS, as four lines.
 *
 * The segment line prints `seg_base[]` as it stands, which is not always
 * the base the machine would use: the cache is rebuilt on demand, and
 * until then it is stale. So the line says which it is:
 *
 *   base=12340    the cache is current and agrees with the register
 *   base=00000*   the cache is marked stale; an access would rebuild it,
 *                 and the number shown is not authoritative
 *   base=99990!   the cache is marked current and does NOT agree with
 *                 the register, which is a bug
 *
 * The distinction is the point of printing both at all. The `8E` load of
 * a segment register is exactly the case where the register and the cache
 * can disagree, and a debugger that quietly recomputed the base from the
 * register would hide the one thing it was asked to show.
 */
void vm86_dbg_dump_state(const struct vm86_cpu *cpu, struct vm86_dbg_out *out);

/*
 * FLAGS as a program would read it: every flag named, and the bits the
 * part hardwires shown as the ones they read as.
 *
 * `flags` is the value in the register. The bits that always read as one
 * are forced on for the display, because that is what a program pushing
 * FLAGS and looking at the result sees, and because that test is how
 * software of the era worked out which CPU it was on. When the register
 * itself has those bits clear -- something no 8086 can produce -- the
 * stored value is printed next to the line, since that is a bug in
 * whatever wrote the register.
 */
void vm86_dbg_dump_flags(uint16_t flags, struct vm86_dbg_out *out);

/*
 * A hex and ASCII dump of guest memory, sixteen bytes to a line.
 *
 * `linear` is a linear address. Every byte comes from vm86_mem_read8(),
 * never from `mem->ram` directly: the 0xFF a floating bus returns, and
 * whatever the display window does to 0xA0000 later, exist only behind
 * that call, and a debugger whose memory disagrees with the emulator's is
 * worse than no debugger.
 */
void vm86_dbg_dump_memory(struct vm86_cpu *cpu, uint32_t linear,
                          uint16_t count, struct vm86_dbg_out *out);

/*
 * One trace line, then the state the instruction left behind.
 *
 * The line is the address, the raw bytes, the length and the outcome; the
 * state below it is vm86_dbg_dump_state() of the recorded machine. No
 * mnemonic: see the note at the top of this file.
 */
void vm86_dbg_dump_trace(const struct vm86_dbg_trace *trace, struct vm86_dbg_out *out);

#endif /* VM86_DBG_H */
