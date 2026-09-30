#include "replay.h"

#include <vm86/ops.h>

/*
 * The replayer.
 *
 * See replay.h for what it is for and for the guest image convention.
 * The one thing worth repeating here is the include list above: it is
 * short because this code has to run in a guest, where there is no stdio
 * and no allocator, and a formatter that reaches for either is a
 * formatter that has to be rewritten before the next milestone.
 */

/* ------------------------------------------------------------------ */
/* Text, without a printf                                              */
/* ------------------------------------------------------------------ */

/*
 * A line under construction.
 *
 * The buffer is on the stack and fixed, because the guest side cannot
 * grow it. Nothing here can overflow it: put_char() drops what does not
 * fit, and every number this file prints is at most sixteen bits wide, so
 * a line is bounded by its own text well below the limit.
 */
struct corpus_emit {
    vm86_corpus_report report;
    void              *ctx;
    char               line[112];
    size_t             used;
};

static void flush(struct corpus_emit *e)
{
    if (e->report)
        e->report(e->ctx, e->line, e->used);

    e->used = 0;
}

static void put_char(struct corpus_emit *e, char c)
{
    if (e->used < sizeof(e->line) - 1u)
        e->line[e->used++] = c;
}

static void put_text(struct corpus_emit *e, const char *text)
{
    while (*text)
        put_char(e, *text++);
}

static void put_hex16(struct corpus_emit *e, uint16_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    put_text(e, "0x");

    for (int shift = 12; shift >= 0; shift -= 4)
        put_char(e, digits[(value >> shift) & 0xFu]);
}

static void put_dec(struct corpus_emit *e, uint32_t value)
{
    char  reversed[10];
    size_t n = 0;

    if (value == 0) {
        put_char(e, '0');
        return;
    }

    while (value != 0) {
        reversed[n++] = (char)('0' + value % 10u);
        value /= 10u;
    }

    while (n > 0)
        put_char(e, reversed[--n]);
}

/* `<what>: expected 0xNNNN, got 0xNNNN`, the shape every difference to a
 * register takes. */
static void report_word(struct corpus_emit *e, const char *what,
                        uint16_t expected, uint16_t got)
{
    put_text(e, what);
    put_text(e, ": expected ");
    put_hex16(e, expected);
    put_text(e, ", got ");
    put_hex16(e, got);
    flush(e);
}

/* The same, for a byte of memory, which also needs to say where. */
static void report_byte(struct corpus_emit *e, uint16_t segment,
                        uint16_t offset, uint8_t expected, uint8_t got)
{
    put_text(e, "memory at ");
    put_hex16(e, segment);
    put_char(e, ':');
    put_hex16(e, offset);
    put_text(e, ": expected ");
    put_hex16(e, (uint16_t)expected);
    put_text(e, ", got ");
    put_hex16(e, (uint16_t)got);
    flush(e);
}

/* ------------------------------------------------------------------ */
/* Comparisons                                                        */
/* ------------------------------------------------------------------ */

static bool same_word(struct corpus_emit *e, const char *what,
                      uint16_t expected, uint16_t got)
{
    if (expected == got)
        return true;

    report_word(e, what, expected, got);
    return false;
}

/*
 * The whole flags register.
 *
 * The bits that always read as one are masked out rather than being
 * written into every expected value: they are not the guest's to control,
 * so a case that had to restate them would be restating a constant, and
 * the constant would then be in two places. `ignore` covers a flag the
 * instruction that wrote last leaves undefined -- see corpus_case.
 */
static bool same_flags(struct corpus_emit *e, uint16_t expected, uint16_t got,
                       uint16_t ignore)
{
    uint16_t mask = (uint16_t)(VM86_FLAG_MASK & ~ignore);

    if ((got & mask) == (expected & mask))
        return true;

    put_text(e, "FLAGS: expected ");
    put_hex16(e, (uint16_t)(expected & mask));
    put_text(e, ", got ");
    put_hex16(e, (uint16_t)(got & mask));
    put_text(e, " (bits compared ");
    put_hex16(e, mask);
    put_text(e, ")");
    flush(e);

    return false;
}

static bool same_memory(struct corpus_emit *e, struct vm86_mem *mem,
                        const struct corpus_memory *want)
{
    bool     same = true;
    uint32_t base = (uint32_t)want->segment << 4;

    for (uint8_t i = 0; i < want->length; i++) {
        uint16_t offset = (uint16_t)(want->offset + i);
        uint8_t  got    = vm86_mem_read8(mem, base + offset);

        if (got == want->bytes[i])
            continue;

        report_byte(e, want->segment, offset, want->bytes[i], got);
        same = false;
    }

    return same;
}

/* ------------------------------------------------------------------ */
/* The run                                                            */
/* ------------------------------------------------------------------ */

/*
 * Put the machine in the state the convention describes.
 *
 * Everything a handler may not do is fine here: this is the outside
 * world, setting the machine up before anyone executes anything. The
 * segment bases are refreshed once rather than on demand because the
 * first instruction will want all four of them.
 */
static void power_on(struct vm86_cpu *cpu, struct vm86_mem *mem,
                     const struct corpus_case *c)
{
    for (uint16_t i = 0; i < c->image_size; i++)
        vm86_mem_write8(mem, VM86_CORPUS_LOAD_OFFSET + i, c->image[i]);

    vm86_reset(cpu, mem);

    vm86_set_seg(cpu, VM86_CS, VM86_CORPUS_LOAD_SEGMENT);
    vm86_set_seg(cpu, VM86_DS, VM86_CORPUS_LOAD_SEGMENT);
    vm86_set_seg(cpu, VM86_ES, VM86_CORPUS_LOAD_SEGMENT);
    vm86_set_seg(cpu, VM86_SS, VM86_CORPUS_LOAD_SEGMENT);
    vm86_flush_segments(cpu);

    cpu->ip    = VM86_CORPUS_LOAD_OFFSET;
    cpu->sp    = VM86_CORPUS_STACK_TOP;
    cpu->flags = VM86_CORPUS_INITIAL_FLAGS;
}

enum corpus_outcome vm86_corpus_replay(struct vm86_mem *mem,
                                       const struct corpus_case *c,
                                       vm86_corpus_report report, void *ctx)
{
    struct corpus_emit e;
    struct vm86_cpu    cpu;
    enum vm86_result   result = VM86_CONTINUE;
    uint32_t           limit;
    uint32_t           steps = 0;

    e.report = report;
    e.ctx    = ctx;
    e.used   = 0;

    vm86_mem_clear(mem);
    power_on(&cpu, mem, c);

    limit = c->insn_limit ? c->insn_limit : VM86_CORPUS_INSN_LIMIT_DEFAULT;

    while (result == VM86_CONTINUE && steps < limit) {
        result = vm86_step(&cpu);
        steps++;
    }

    /*
     * Out of budget. Said in the words of the thing that failed: a reader
     * who is told the state did not match will go looking for the wrong
     * arithmetic, and there is none. The step count goes in because the
     * two usual causes -- a sample that never ends and a limit set too
     * low -- are told apart by whether the machine got anywhere.
     */
    if (result == VM86_CONTINUE) {
        put_text(&e, "did not stop within ");
        put_dec(&e, limit);
        put_text(&e, " instructions (this is the instruction limit, not a "
                     "state mismatch)");
        flush(&e);
        return CORPUS_INSN_LIMIT;
    }

    if (result == VM86_FAULT) {
        if (c->ending != CORPUS_ENDS_FAULTED) {
            put_text(&e, "the machine faulted (vector ");
            put_dec(&e, cpu.fault);
            put_text(&e, ") where the case expects it to halt");
            flush(&e);
            return CORPUS_WRONG_ENDING;
        }

        if (cpu.fault != c->fault) {
            report_word(&e, "fault vector", c->fault, cpu.fault);
            return CORPUS_WRONG_FAULT;
        }

        return CORPUS_PASS;
    }

    /*
     * The emulator failed rather than the sample. The fault is in the
     * host and is reported in the host's words: "the machine halted where
     * the case expects a fault" is what the branch below would say, and
     * it would send the reader looking for a missing HLT in a sample that
     * is fine.
     */
    if (result == VM86_INTERNAL_ERROR) {
        put_text(&e, "the interpreter's opcode table did not merge, so "
                     "nothing in this run reached the handler it should "
                     "have and this case has not been judged");
        flush(&e);
        return CORPUS_INTERNAL_ERROR;
    }

    /*
     * VM86_HALT. It is the only ending left: every other value the run
     * loop can return is handled above, so this is no longer a catch-all
     * that happens to hold a halt -- which is what it was written as.
     */
    if (c->ending != CORPUS_ENDS_HALTED) {
        put_text(&e, "the machine halted where the case expects a fault "
                     "(vector ");
        put_dec(&e, c->fault);
        put_text(&e, ")");
        flush(&e);
        return CORPUS_WRONG_ENDING;
    }

    /*
     * HLT is the only thing that stops the machine without faulting, and
     * it is supposed to say so in the register as well as in the return
     * value. Worth a line because the two are separate fields and a
     * handler that sets one and forgets the other would otherwise be
     * invisible until something resumed the processor.
     */
    if (!cpu.halted) {
        put_text(&e, "stopped without setting cpu->halted");
        flush(&e);
        return CORPUS_WRONG_ENDING;
    }

    bool same = true;

    same &= same_word(&e, "AX", c->final.ax, cpu.ax);
    same &= same_word(&e, "BX", c->final.bx, cpu.bx);
    same &= same_word(&e, "CX", c->final.cx, cpu.cx);
    same &= same_word(&e, "DX", c->final.dx, cpu.dx);
    same &= same_word(&e, "SI", c->final.si, cpu.si);
    same &= same_word(&e, "DI", c->final.di, cpu.di);
    same &= same_word(&e, "BP", c->final.bp, cpu.bp);
    same &= same_word(&e, "SP", c->final.sp, cpu.sp);

    same &= same_word(&e, "CS", c->final.cs, cpu.cs);
    same &= same_word(&e, "DS", c->final.ds, cpu.ds);
    same &= same_word(&e, "ES", c->final.es, cpu.es);
    same &= same_word(&e, "SS", c->final.ss, cpu.ss);
    same &= same_word(&e, "IP", c->final.ip, cpu.ip);

    same &= same_flags(&e, c->final.flags, cpu.flags, c->flags_ignore);

    for (uint8_t i = 0; i < c->memory_count; i++)
        same &= same_memory(&e, mem, &c->memory[i]);

    return same ? CORPUS_PASS : CORPUS_MISMATCH;
}
