/*
 * The single-step debugger: decoding, stepping, formatting.
 *
 * The header explains why the output is a callback and why the length is
 * decoded rather than measured. What is here is the two halves of that:
 * an instruction-length decoder written against the opcode map, and a set
 * of formatters that build one line at a time into a stack buffer.
 *
 * This file is freestanding. Its includes are the three standard integer
 * and size headers plus vm86/dbg.h, and nothing in it allocates, prints,
 * or remembers anything between calls. That is not a style preference: it
 * is what lets the same object run in a host test and in a user-space
 * process inside FunnyOS.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vm86/dbg.h>
#include <vm86/mem.h>

/* ------------------------------------------------------------------ */
/* Reading the instruction stream                                      */
/* ------------------------------------------------------------------ */

/*
 * A byte of the code, read without moving anything.
 *
 * This is the whole reason the length can be worked out ahead of the
 * step. vm86_fetch8() would advance IP past the byte, which is fine while
 * executing and useless afterwards: by the time the instruction has run,
 * the only record of where it started is a number somebody remembered to
 * save, and the only way to measure its length is a subtraction that does
 * not work.
 */
static uint8_t peek_code(struct vm86_cpu *cpu, uint16_t offset)
{
    return vm86_mem_read8(cpu->mem, vm86_linear(cpu, VM86_CS, offset));
}

/*
 * The bytes step.c consumes as prefixes.
 *
 * THIS HAS TO AGREE WITH step.c EXACTLY. One byte of disagreement and
 * every instruction that begins with a prefix gets the wrong length, and
 * the wrong length is not a wrong mnemonic -- it is a trace that reads
 * the operand bytes of the next instruction as part of this one.
 */
static bool is_prefix(uint8_t byte)
{
    switch (byte) {
    case 0x26:                      /* es: */
    case 0x2E:                      /* cs: */
    case 0x36:                      /* ss: */
    case 0x3E:                      /* ds: */
    case 0xF0:                      /* lock */
    case 0xF2:                      /* repne */
    case 0xF3:                      /* rep */
        return true;
    default:
        return false;
    }
}

/*
 * Whether any group claims this opcode.
 *
 * The merged table is what the run loop dispatches through, and ops.h
 * declares it "for tests and for the trace dump" -- this is the trace
 * dump. Asking it rather than repeating the list means the decoder is
 * right about the opcodes nobody implements without a second table to
 * keep in step: 63 is ARPL on a 286, D8-DF escape to an 8087, F1 is
 * nothing at all, and on this machine all of them are one byte that
 * raises the invalid-opcode exception.
 */
static bool is_claimed(uint8_t opcode)
{
    const vm86_op_fn *table = vm86_ops_table();

    if (!table) {
        /*
         * Nothing has run yet, so the groups have not been merged.
         * Building them is idempotent and is what the run loop itself
         * does, so a trace taken before the first instruction decodes
         * exactly the way the instruction will run.
         */
        vm86_ops_build();
        table = vm86_ops_table();
    }

    /*
     * Still no table: two groups claimed the same opcode. The harness
     * reports that before it runs anything, and when it is true every
     * test is unreliable anyway. Assuming "one byte" for all 256 opcodes
     * would be the worse answer, so this falls back to the encoding.
     */
    if (!table)
        return true;

    return table[opcode] != NULL;
}

/* ------------------------------------------------------------------ */
/* The instruction length                                              */
/* ------------------------------------------------------------------ */

/*
 * What the ModRM byte brings with it: itself, and a displacement if its
 * mode says so.
 *
 * Mod 0 with r/m 6 is the odd one: instead of a base register it carries
 * a direct sixteen-bit address, so it has a displacement with no register
 * to add it to. It is the same size as mod 2 and nothing like it in
 * meaning, which is the kind of thing that gets a length decoder right
 * for every form but one.
 */
static uint16_t modrm_bytes(uint8_t byte)
{
    switch ((byte >> 6) & 3u) {
    case 0:  return ((byte & 7u) == 6u) ? 3u : 1u;
    case 1:  return 2u;
    case 2:  return 3u;
    default: return 1u;      /* mod 3 is a register, with no displacement */
    }
}

/*
 * How many bytes follow the opcode.
 *
 * Written as the opcode map rather than as a table of 256 numbers,
 * because the map has structure and the structure is the documentation.
 * Four stretches of it are regular -- the arithmetic block, the register
 * block, the conditional jumps, the immediate moves -- and those are
 * handled a stretch at a time. Everything irregular is spelled out, which
 * is the honest shape of this processor's encoding: it is regular where
 * it was convenient in 1978 and ad hoc everywhere else.
 *
 * The immediate-sizes here are the ones this emulator's handlers consume.
 * If a handler and this table ever disagree the trace says so rather than
 * silently dumping the wrong bytes; see ip_advance_ok.
 */
static uint16_t operand_bytes(struct vm86_cpu *cpu, uint8_t opcode,
                              uint16_t at, bool *transfer)
{
    *transfer = false;

    uint8_t modrm = peek_code(cpu, at);
    uint8_t reg   = (uint8_t)((modrm >> 3) & 7u);

    /*
     * The arithmetic block: eight operations in six forms each, and the
     * two leftover slots per operation are not all leftovers -- the low
     * half of them hold the segment pushes and the decimal adjustments.
     */
    if (opcode <= 0x3F) {
        switch (opcode & 7u) {
        case 0: case 1: case 2: case 3: return modrm_bytes(modrm);
        case 4:                         return 1u;   /* AL, imm8 */
        case 5:                         return 2u;   /* AX, imm16 */
        default:                        return 0u;   /* push/pop segreg,
                                                      * DAA, DAS, AAA, AAS */
        }
    }

    /* INC/DEC/PUSH/POP r16: the register is inside the opcode, so the
     * instruction is the opcode -- thirty-two of them in a row. */
    if (opcode >= 0x40 && opcode <= 0x5F)
        return 0u;

    /* The conditional jumps: one signed byte of displacement. */
    if (opcode >= 0x70 && opcode <= 0x7F) {
        *transfer = true;
        return 1u;
    }

    /* MOV to a register from an immediate: the register is in the opcode
     * and only the width varies. */
    if (opcode >= 0xB0 && opcode <= 0xB7) return 1u;
    if (opcode >= 0xB8 && opcode <= 0xBF) return 2u;

    switch (opcode) {
    /* --- the 186 additions, and the bytes around them --- */
    case 0x60: case 0x61:   return 0u;      /* PUSHA, POPA */
    case 0x62:              return modrm_bytes(modrm);   /* BOUND */
    case 0x63: case 0x64:
    case 0x65: case 0x66:
    case 0x67:              return 0u;      /* nothing on this machine */
    case 0x68:              return 2u;      /* push imm16 */
    case 0x69:              return (uint16_t)(modrm_bytes(modrm) + 2u);
    case 0x6A:              return 1u;      /* push imm8 */
    case 0x6B:              return (uint16_t)(modrm_bytes(modrm) + 1u);
    case 0x6C: case 0x6D:
    case 0x6E: case 0x6F:   return 0u;      /* INS, OUTS */

    /* --- group 1: the arithmetic with an immediate --- */
    case 0x80: case 0x82:   return (uint16_t)(modrm_bytes(modrm) + 1u);
    case 0x81:              return (uint16_t)(modrm_bytes(modrm) + 2u);
    case 0x83:              return (uint16_t)(modrm_bytes(modrm) + 1u);

    /* --- TEST through POP: ModRM and nothing else --- */
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
                            return modrm_bytes(modrm);

    /* --- the one-byte stretch, with the far call inside it --- */
    case 0x9A:              *transfer = true; return 4u;
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9B: case 0x9C:
    case 0x9D: case 0x9E: case 0x9F:
                            return 0u;

    /* --- the direct addresses, then the string instructions --- */
    case 0xA0: case 0xA1:
    case 0xA2: case 0xA3:   return 2u;      /* the moffs is a word */
    case 0xA8:              return 1u;
    case 0xA9:              return 2u;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD:
    case 0xAE: case 0xAF:   return 0u;

    /* --- shifts, returns, and the loads of a far pointer --- */
    case 0xC0: case 0xC1:   return (uint16_t)(modrm_bytes(modrm) + 1u);
    case 0xC2:              *transfer = true; return 2u;   /* ret imm16 */
    case 0xC3:              *transfer = true; return 0u;   /* ret */
    case 0xC4: case 0xC5:   return modrm_bytes(modrm);     /* LES, LDS */
    case 0xC6:              return (uint16_t)(modrm_bytes(modrm) + 1u);
    case 0xC7:              return (uint16_t)(modrm_bytes(modrm) + 2u);
    case 0xC8:              return 3u;      /* enter: alloc word, level byte */
    case 0xC9:              return 0u;      /* leave */
    case 0xCA:              *transfer = true; return 2u;   /* retf imm16 */
    case 0xCB:              *transfer = true; return 0u;   /* retf */
    case 0xCC:              *transfer = true; return 0u;   /* int3 */
    case 0xCD:              *transfer = true; return 1u;   /* int imm8 */
    case 0xCE:              *transfer = true; return 0u;   /* into */
    case 0xCF:              *transfer = true; return 0u;   /* iret */

    /* --- the rest of the shifts, the decimal adjusts, the escapes --- */
    case 0xD0: case 0xD1:
    case 0xD2: case 0xD3:   return modrm_bytes(modrm);
    case 0xD4: case 0xD5:   return 1u;      /* AAM and AAD carry a base */
    case 0xD6: case 0xD7:   return 0u;      /* SALC is nothing; XLAT is one */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
                            return 0u;      /* 8087 escapes, which are M7.
                                             * On this part they are one
                                             * byte that faults, and this
                                             * table has to describe this
                                             * part. M7 will have to extend
                                             * it when it claims them. */

    /* --- the loops, then the port instructions --- */
    case 0xE0: case 0xE1:
    case 0xE2: case 0xE3:   *transfer = true; return 1u;
    case 0xE4: case 0xE5:
    case 0xE6: case 0xE7:   return 1u;      /* the port number is a byte */
    case 0xE8: case 0xE9:   *transfer = true; return 2u;
    case 0xEA:              *transfer = true; return 4u;
    case 0xEB:              *transfer = true; return 1u;
    case 0xEC: case 0xED:
    case 0xEE: case 0xEF:   return 0u;      /* the port number is in DX */

    /* --- the flag instructions, and the two groups that end the map --- */
    case 0xF1: case 0xF4: case 0xF5:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD:   return 0u;

    /*
     * Group 3 is the one place in the map where the ModRM byte does not
     * settle the length: the immediate belongs to TEST, and the other six
     * sub-operations -- NOT, NEG, MUL, IMUL, DIV, IDIV -- have none. So
     * the register field is read out of the ModRM byte before the
     * length can be known.
     */
    case 0xF6: return (uint16_t)(modrm_bytes(modrm) + ((reg <= 1u) ? 1u : 0u));
    case 0xF7: return (uint16_t)(modrm_bytes(modrm) + ((reg <= 1u) ? 2u : 0u));

    /* Group 4 is INC and DEC in both widths, and nothing else. */
    case 0xFE:              return modrm_bytes(modrm);

    /*
     * Group 5 is INC, DEC and PUSH, which leave the pointer alone, next
     * to the four call and jump forms, which do not. The register field
     * is what tells them apart, so the byte has to be read first.
     */
    case 0xFF:
        *transfer = (reg >= 2u && reg <= 5u);
        return modrm_bytes(modrm);

    default:
        /* The prefix bytes land here if the scan above ever gives up on
         * them; they are consumed in the instruction stream and never
         * dispatched. */
        return 0u;
    }
}

void vm86_dbg_decode(struct vm86_cpu *cpu, struct vm86_dbg_decode *out)
{
    out->prefixes = 0;
    out->transfers_control = false;

    uint16_t at = cpu->ip;

    while (out->prefixes < VM86_DBG_MAX_PREFIX && is_prefix(peek_code(cpu, at))) {
        at = (uint16_t)(at + 1u);
        out->prefixes++;
    }

    uint8_t opcode = peek_code(cpu, at);
    at = (uint16_t)(at + 1u);

    bool     transfer = false;
    uint16_t operands = 0;

    if (is_claimed(opcode))
        operands = operand_bytes(cpu, opcode, at, &transfer);

    out->length            = (uint8_t)(out->prefixes + 1u + operands);
    out->transfers_control = transfer;
}

/* ------------------------------------------------------------------ */
/* The step                                                            */
/* ------------------------------------------------------------------ */

enum vm86_result vm86_dbg_step(struct vm86_cpu *cpu, struct vm86_dbg_trace *trace)
{
    struct vm86_dbg_decode decoded;

    /*
     * Everything that describes the instruction is taken before it runs.
     * Afterwards the pointer has moved, the bytes at CS:IP belong to
     * whatever comes next, and for a jump they belong to somewhere else
     * entirely.
     */
    vm86_dbg_decode(cpu, &decoded);

    uint16_t start_ip = cpu->ip;
    uint16_t start_cs = cpu->cs;

    enum vm86_result result = vm86_step(cpu);

    if (!trace)
        return result;

    trace->cs               = start_cs;
    trace->ip               = start_ip;
    trace->length           = decoded.length;
    trace->prefixes         = decoded.prefixes;
    trace->transfers_control = decoded.transfers_control;

    trace->shown = (decoded.length < VM86_DBG_MAX_BYTES)
                 ? decoded.length
                 : (uint8_t)VM86_DBG_MAX_BYTES;

    /*
     * The bytes come out of the instruction stream, read one at a time
     * through the memory layer. Reading them from a buffer would be
     * faster and would show something the emulator never saw, which is
     * the one thing a debugger must not do.
     */
    for (uint8_t i = 0; i < trace->shown; i++)
        trace->bytes[i] = peek_code(cpu, (uint16_t)(start_ip + i));

    trace->result = result;
    trace->fault  = (result == VM86_FAULT) ? cpu->fault : 0;
    trace->after  = *cpu;

    /*
     * Did the pointer end up where the decoder said it should?
     *
     * For everything but a control transfer the answer has to be yes --
     * including for an instruction that faulted, and including a repeated
     * string instruction with a count of zero, which still consumed its
     * prefix and its opcode. A "no" here means this file and the emulator
     * disagree about where the instruction ends, and the trace says so
     * instead of printing bytes that were never executed.
     *
     * The subtraction is done in sixteen bits, because that is the width
     * IP has: a forward jump past the segment boundary wraps rather than
     * growing, and a backward jump is a small negative number.
     */
    int16_t advance = (int16_t)(uint16_t)(trace->after.ip - start_ip);

    trace->ip_advance_ok = trace->transfers_control
                        || (advance == (int16_t)decoded.length);

    return result;
}

/* ------------------------------------------------------------------ */
/* Building a line                                                     */
/* ------------------------------------------------------------------ */

/*
 * One line, and somewhere to put it.
 *
 * The buffer is a local of whatever formatter is running and never
 * outlives it. Nothing is allocated; nothing is kept. 160 bytes is a
 * little over twice the widest line any formatter here builds, which is
 * the state dump's flags line with the note about a stored value that
 * disagrees attached to it.
 */
#define DBG_LINE_MAX 160u

struct line {
    struct vm86_dbg_out *out;
    size_t               n;
    char                 buf[DBG_LINE_MAX];
};

static char hex_digit(uint32_t value)
{
    return (char)(value < 10u ? '0' + value : 'A' + (value - 10u));
}

static void line_begin(struct line *l, struct vm86_dbg_out *out)
{
    l->out = out;
    l->n   = 0;
}

/*
 * A character, unless the line is already full.
 *
 * Truncating is the wrong answer in general and the right one here: the
 * buffer is sized so that no line this file builds comes near it, and a
 * formatter that overflowed a stack buffer to print a register would be
 * a much worse bug than a short line.
 */
static void line_char(struct line *l, char c)
{
    if (l->n < DBG_LINE_MAX)
        l->buf[l->n++] = c;
}

static void line_text(struct line *l, const char *text)
{
    for (const char *p = text; *p; p++)
        line_char(l, *p);
}

static void line_hex(struct line *l, uint32_t value, unsigned digits)
{
    for (unsigned shift = digits * 4u; shift > 0u; shift -= 4u)
        line_char(l, hex_digit((value >> (shift - 4u)) & 0xFu));
}

static void line_dec(struct line *l, uint32_t value)
{
    char     digits[10];
    unsigned n = 0;

    do {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && n < sizeof(digits));

    while (n > 0u)
        line_char(l, digits[--n]);
}

static void line_end(struct line *l)
{
    line_char(l, '\n');

    if (l->out && l->out->write)
        l->out->write(l->out->ctx, l->buf, l->n);
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

static const char *result_name(enum vm86_result result)
{
    switch (result) {
    case VM86_CONTINUE: return "continue";
    case VM86_HALT:     return "halt";
    case VM86_FAULT:    return "fault";
    }

    return "unknown";
}

static void flag_bit(struct line *l, const char *name, uint16_t flags, uint16_t bit)
{
    line_text(l, name);
    line_char(l, '=');
    line_char(l, (flags & bit) ? '1' : '0');
    line_char(l, ' ');
}

static void flags_into(struct line *l, uint16_t flags)
{
    /*
     * What a program would read, not what is in the register.
     *
     * Bit 1 is set on every x86 ever made and bits 12-15 read as one on
     * the 8086, and a program of the era worked out what it was running
     * on by pushing FLAGS and looking at them -- "if F000 comes back,
     * this is an 8086". A debugger that printed the register verbatim
     * would make that test look like a bug in the emulator, which is
     * exactly backwards: the emulator is being faithful and the display
     * would be the thing that lies.
     *
     * The register's own value is printed when the two disagree. No 8086
     * can produce that state, so it means something wrote the register
     * without normalising it -- which is worth saying out loud, because
     * the visible symptom otherwise is a program that misdetects the CPU.
     */
    uint16_t shown = (uint16_t)((flags & VM86_FLAG_MASK) | VM86_FLAG_ALWAYS_SET);

    line_text(l, "FLAGS ");
    line_hex(l, shown, 4u);
    line_text(l, "  ");

    flag_bit(l, "CF", shown, VM86_CF);
    flag_bit(l, "PF", shown, VM86_PF);
    flag_bit(l, "AF", shown, VM86_AF);
    flag_bit(l, "ZF", shown, VM86_ZF);
    flag_bit(l, "SF", shown, VM86_SF);
    flag_bit(l, "TF", shown, VM86_TF);
    flag_bit(l, "IF", shown, VM86_IF);
    flag_bit(l, "DF", shown, VM86_DF);
    flag_bit(l, "OF", shown, VM86_OF);

    /* The reserved bits, named as what they are rather than folded into
     * the nine the guest controls. */
    line_text(l, " b1=");
    line_char(l, (shown & 0x0002u) ? '1' : '0');
    line_text(l, " b12-15=");
    for (unsigned bit = 15u; bit >= 12u; bit--)
        line_char(l, (shown & (1u << bit)) ? '1' : '0');

    if ((flags & VM86_FLAG_ALWAYS_SET) != VM86_FLAG_ALWAYS_SET) {
        line_text(l, "  [stored ");
        line_hex(l, flags, 4u);
        line_text(l, " has the always-set bits clear]");
    }
}

static void segment_into(struct line *l, const struct vm86_cpu *cpu,
                         const char *name, enum vm86_seg which)
{
    uint16_t value = vm86_get_seg(cpu, which);
    uint32_t base  = cpu->seg_base[which];
    bool     stale = (cpu->seg_dirty & (uint8_t)(1u << which)) != 0;

    line_text(l, name);
    line_char(l, '=');
    line_hex(l, value, 4u);
    line_text(l, " base=");
    line_hex(l, base, 5u);

    if (stale)
        line_char(l, '*');      /* not rebuilt yet; an access would */
    else if (base != ((uint32_t)value << 4))
        line_char(l, '!');      /* current, and disagreeing: a bug */
}

void vm86_dbg_dump_state(const struct vm86_cpu *cpu, struct vm86_dbg_out *out)
{
    struct line l;

    line_begin(&l, out);
    line_text(&l, "AX="); line_hex(&l, cpu->ax, 4u);
    line_text(&l, " BX="); line_hex(&l, cpu->bx, 4u);
    line_text(&l, " CX="); line_hex(&l, cpu->cx, 4u);
    line_text(&l, " DX="); line_hex(&l, cpu->dx, 4u);
    line_end(&l);

    line_begin(&l, out);
    line_text(&l, "SP="); line_hex(&l, cpu->sp, 4u);
    line_text(&l, " BP="); line_hex(&l, cpu->bp, 4u);
    line_text(&l, " SI="); line_hex(&l, cpu->si, 4u);
    line_text(&l, " DI="); line_hex(&l, cpu->di, 4u);
    line_end(&l);

    line_begin(&l, out);
    segment_into(&l, cpu, "CS", VM86_CS); line_text(&l, "  ");
    segment_into(&l, cpu, "DS", VM86_DS); line_text(&l, "  ");
    segment_into(&l, cpu, "ES", VM86_ES); line_text(&l, "  ");
    segment_into(&l, cpu, "SS", VM86_SS);
    line_end(&l);

    line_begin(&l, out);
    line_text(&l, "IP="); line_hex(&l, cpu->ip, 4u);
    line_char(&l, ' ');
    flags_into(&l, cpu->flags);
    line_end(&l);
}

void vm86_dbg_dump_flags(uint16_t flags, struct vm86_dbg_out *out)
{
    struct line l;

    line_begin(&l, out);
    flags_into(&l, flags);
    line_end(&l);
}

/*
 * How many hex digits an address needs, at least five.
 *
 * Five covers the 8086's megabyte, which is what almost every dump will
 * be inside. Past the end of it a fixed five would wrap: a line at
 * 0x100008 would print as `00008` and read as a low address, which is the
 * one kind of mistake a memory dump must not make. Five is the floor
 * because that is the conventional width; the extra digits appear only
 * when the address actually has them.
 */
static unsigned address_digits(uint32_t value)
{
    unsigned digits = 5u;

    while (digits < 8u && (value >> (digits * 4u)) != 0u)
        digits++;

    return digits;
}

void vm86_dbg_dump_memory(struct vm86_cpu *cpu, uint32_t linear,
                          uint16_t count, struct vm86_dbg_out *out)
{
    struct line l;
    uint8_t     row[16];
    uint32_t    total = count;

    for (uint32_t done = 0; done < total; done += 16u) {
        uint32_t n = total - done;
        if (n > 16u)
            n = 16u;

        /* Each byte is read once and used twice. Reading it twice would
         * count it twice in the memory layer's statistics for unmapped
         * addresses, and those counters are how a program that does
         * nothing but probe is meant to be noticed. */
        for (uint32_t i = 0; i < n; i++)
            row[i] = vm86_mem_read8(cpu->mem, linear + done + i);

        line_begin(&l, out);
        line_hex(&l, linear + done, address_digits(linear + done));
        line_text(&l, "  ");

        for (uint32_t i = 0; i < 16u; i++) {
            if (i < n)
                line_hex(&l, row[i], 2u);
            else
                line_text(&l, "  ");
            line_char(&l, ' ');
        }

        line_char(&l, '|');
        for (uint32_t i = 0; i < n; i++)
            line_char(&l, (row[i] >= 0x20 && row[i] <= 0x7E) ? (char)row[i] : '.');
        for (uint32_t i = n; i < 16u; i++)
            line_char(&l, ' ');
        line_char(&l, '|');

        line_end(&l);
    }
}

void vm86_dbg_dump_trace(const struct vm86_dbg_trace *trace, struct vm86_dbg_out *out)
{
    struct line l;

    line_begin(&l, out);

    line_hex(&l, trace->cs, 4u);
    line_char(&l, ':');
    line_hex(&l, trace->ip, 4u);
    line_text(&l, "  ");

    for (uint8_t i = 0; i < trace->shown; i++) {
        line_hex(&l, trace->bytes[i], 2u);
        line_char(&l, ' ');
    }

    /* An instruction with more prefixes than the record keeps bytes for
     * is longer than it looks. Saying so is better than printing a short
     * list that reads as the whole instruction. */
    if (trace->shown < trace->length)
        line_text(&l, "..");

    /* The summary starts at a fixed column, so that a page of trace can
     * be read down the middle rather than hunted through. */
    while (l.n < 60u)
        line_char(&l, ' ');

    line_char(&l, '(');
    line_dec(&l, trace->length);
    line_text(&l, ")  ");
    line_text(&l, result_name(trace->result));

    if (trace->result == VM86_FAULT) {
        line_text(&l, "  fault ");
        line_hex(&l, trace->fault, 2u);
    }

    if (!trace->ip_advance_ok)
        line_text(&l, "  [!] the ip did not move by the decoded length");

    line_end(&l);

    vm86_dbg_dump_state(&trace->after, out);
}
