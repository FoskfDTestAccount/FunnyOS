#include "bios16.h"

#include <vm86/firmware.h>
#include <vm86/host.h>
#include <vm86/mem.h>

/*
 * ---------------------------------------------------------------------
 * Where the numbers come from
 *
 * None of this is in the 8086 manual. The scan code table, the ring
 * buffer layout and the shift flag bits are the IBM PC BIOS, published in
 * the IBM PC Technical Reference and copied by everybody since. This
 * repository has neither that document nor Ralf Brown's list, so every
 * table below carries a note saying how confident it is -- the same rule
 * the milestone's task books are written under. Two of them are marked
 * as second-hand and one as a choice, and those three are the ones to
 * check first if a keyboard-using program misbehaves.
 * ---------------------------------------------------------------------
 */

/* ------------------------------------------------------------------ */
/* Data area addresses                                                 */
/* ------------------------------------------------------------------ */

/*
 * 0040:0071, bit 7: the keyboard buffer overflowed and a keystroke was
 * discarded.
 *
 * This offset is NOT in firmware.h's map, and it is not in the task
 * book's list of fields this module owns either -- but the task book's
 * pitfalls section asks for it by number, it is the only byte in the
 * data area nothing else writes, and the flag is what a program checks
 * to notice that it is not reading keys fast enough. Recorded here
 * rather than silently dropped.
 */
#define BIOS16_BDA_OVERFLOW      0x0071u
#define BIOS16_OVERFLOW_BUFFER_FULL 0x80u

/* 0040:0017 bits that are not shift keys: the lock keys. They cannot be
 * derived from what is held, because they stay set after the key comes
 * back up. */
#define BIOS16_LOCK_SCROLL 0x10u
#define BIOS16_LOCK_NUM    0x20u
#define BIOS16_LOCK_CAPS   0x40u
#define BIOS16_LOCK_INS    0x80u

/* 0040:0018, the second shift byte. Only the two bits a PC/XT-class
 * keyboard can set are modelled; see kb_write_shift_flags(). */
#define BIOS16_FLAGS2_LEFT_CTRL 0x01u
#define BIOS16_FLAGS2_LEFT_ALT  0x02u

static uint32_t bda_linear(uint16_t offset)
{
    return ((uint32_t)VM86_BDA_SEGMENT << 4) + offset;
}

/* ------------------------------------------------------------------ */
/* The ring buffer                                                     */
/* ------------------------------------------------------------------ */

/*
 * The buffer runs from 0x1E to 0x3E, and the pointers count in bytes
 * relative to the segment. 0x3E is one past the last entry, so the last
 * entry starts at 0x3C and the next offset after it is the first.
 */
static uint16_t kb_advance(uint16_t offset)
{
    offset = (uint16_t)(offset + 2u);

    return (offset >= VM86_BDA_KB_BUFFER_END) ? (uint16_t)VM86_BDA_KB_BUFFER
                                              : offset;
}

static uint16_t kb_head(struct vm86_mem *mem)
{
    return vm86_mem_read16(mem, bda_linear(VM86_BDA_KB_HEAD));
}

static uint16_t kb_tail(struct vm86_mem *mem)
{
    return vm86_mem_read16(mem, bda_linear(VM86_BDA_KB_TAIL));
}

/*
 * Head == tail means empty. It also means full, one wrap later -- the
 * sixteen entry ring therefore holds fifteen keystrokes, and the last
 * slot is spent on telling the two states apart. That is how the
 * firmware is built, not a shortcut taken here: a program that follows
 * the published rules expects this exact arithmetic, and an
 * implementation clever enough to use all sixteen slots would disagree
 * with it about when the buffer is empty.
 */
static bool kb_empty(struct vm86_mem *mem)
{
    return kb_head(mem) == kb_tail(mem);
}

static void kb_push(struct vm86_cpu *cpu, uint16_t word)
{
    struct vm86_mem *mem = cpu->mem;
    uint16_t tail = kb_tail(mem);
    uint16_t next = kb_advance(tail);

    if (next == kb_head(mem)) {
        /*
         * Full. The incoming keystroke is thrown away rather than
         * overwriting the oldest one: losing the newest key is
         * recoverable by the person typing, and corrupting the ring is
         * not. The overflow is flagged so a program that cares can see
         * it, and on real hardware the speaker clicks.
         */
        uint8_t flags = vm86_mem_read8(mem, bda_linear(BIOS16_BDA_OVERFLOW));

        vm86_mem_write8(mem, bda_linear(BIOS16_BDA_OVERFLOW),
                        (uint8_t)(flags | BIOS16_OVERFLOW_BUFFER_FULL));
        return;
    }

    vm86_mem_write16(mem, bda_linear(tail), word);
    vm86_mem_write16(mem, bda_linear(VM86_BDA_KB_TAIL), next);
}

static bool kb_take(struct vm86_cpu *cpu, uint16_t *word)
{
    struct vm86_mem *mem = cpu->mem;
    uint16_t head = kb_head(mem);

    if (kb_empty(mem))
        return false;

    *word = vm86_mem_read16(mem, bda_linear(head));
    vm86_mem_write16(mem, bda_linear(VM86_BDA_KB_HEAD), kb_advance(head));

    return true;
}

static bool kb_peek(struct vm86_cpu *cpu, uint16_t *word)
{
    struct vm86_mem *mem = cpu->mem;

    if (kb_empty(mem))
        return false;

    *word = vm86_mem_read16(mem, bda_linear(kb_head(mem)));

    return true;
}

/* ------------------------------------------------------------------ */
/* The shift flags                                                     */
/* ------------------------------------------------------------------ */

static uint8_t kb_read_flags(struct vm86_cpu *cpu)
{
    return vm86_mem_read8(cpu->mem,
                          bda_linear(VM86_BDA_KEYBOARD_FLAGS));
}

static void kb_toggle_flag(struct vm86_cpu *cpu, uint8_t bit)
{
    uint32_t at = bda_linear(VM86_BDA_KEYBOARD_FLAGS);
    uint8_t  flags = vm86_mem_read8(cpu->mem, at);

    vm86_mem_write8(cpu->mem, at, (uint8_t)(flags ^ bit));
}

/*
 * Publish the held shift keys into the data area.
 *
 * Both a press and a release get here, which is the point: a program
 * that watches 0040:0017 to find out whether Shift is down is asking a
 * question that has to be answered the moment the key comes back up, not
 * only when it goes down.
 *
 * The low four bits are read-modify-written rather than assigned,
 * because the high four belong to the lock keys, which are still set
 * after their key is released and so cannot be recomputed from what is
 * held.
 *
 * 0040:0018 is the second shift byte. Its bit assignment is the part of
 * this file with the weakest authority: sources disagree about bits 2
 * and 3 (SysReq/Pause against right Ctrl/right Alt). A PC/XT-class
 * keyboard has one Ctrl and one Alt and neither of those keys, so only
 * bits 0 and 1 are written, and they are the two every source agrees on.
 */
static void kb_write_shift_flags(struct vm86_cpu *cpu, const struct bios16_state *st)
{
    struct vm86_mem *mem = cpu->mem;
    uint32_t at = bda_linear(VM86_BDA_KEYBOARD_FLAGS);
    uint8_t  flags = vm86_mem_read8(mem, at);

    vm86_mem_write8(mem, at, (uint8_t)((flags & 0xF0u) | st->held));

    uint8_t second = 0;

    if (st->held & BIOS16_HOLD_CTRL)
        second |= BIOS16_FLAGS2_LEFT_CTRL;
    if (st->held & BIOS16_HOLD_ALT)
        second |= BIOS16_FLAGS2_LEFT_ALT;

    vm86_mem_write8(mem, bda_linear(VM86_BDA_KEYBOARD_FLAGS2), second);
}

/* ------------------------------------------------------------------ */
/* The translation table                                               */
/* ------------------------------------------------------------------ */

/*
 * What a scan code does.
 *
 * K_CHAR and K_LETTER and K_KEYPAD all end up in the buffer; the
 * difference is which column of the entry the character is read from.
 * K_SHIFT and K_LOCK never reach the buffer at all -- a program that
 * reads keys does not get a keystroke for holding Shift, and the firmware
 * does not produce one either.
 */
enum key_kind {
    K_NONE = 0,
    K_CHAR,     /* plain key: base, shifted, or with Ctrl */
    K_LETTER,   /* base is the letter, Caps Lock acts as Shift */
    K_KEYPAD,   /* the numeric keypad: Num Lock acts as Shift */
    K_SHIFT,    /* shift, ctrl or alt: held while down, never buffered */
    K_LOCK,     /* caps, num, scroll: toggles, never buffered */
};

struct key_def {
    enum key_kind kind;
    uint8_t hold;     /* K_SHIFT: the BIOS16_HOLD_* bit to set   */
    uint8_t toggle;   /* 0040:0017 bit to flip on a make code    */
    uint8_t base;     /* character with nothing held, 0 = none   */
    uint8_t shift;    /* character with Shift (or the lock half) */
    uint8_t control;  /* character with Ctrl, 0 = none           */
};

/*
 * Scan code set 1, the PC and PC/XT set, indexed by the make code.
 *
 * The break code for every key is its make code with 0x80 set, which is
 * why this table stops at 0x7F and why the handler masks the top bit off
 * before it looks anything up.
 *
 * The layout is the US keyboard the machine was designed around, and the
 * scan codes themselves are the ones every emulator and every reference
 * agrees on: the break code for a key is its make code with 0x80 added,
 * and the base codes run in physical order from 0x01.
 *
 * The characters come from a single secondary source, because
 * dos-refs.md section 9 puts this table outside its scope -- it is
 * hundreds of rows and belongs to the implementation. The source is
 * HelpPC's "INT 16 - Keyboard Scan Codes", whose rows read as
 * `scancode` then `ASCII`; it was cross-checked against two book tables
 * for the letters and against the IBM PC Technical Reference's own
 * statement about break codes. It is a secondary source. Two cells are
 * worth naming because they are the ones that differ from folklore:
 *
 *   - Shift-Tab produces no character at all, not a second Tab;
 *   - Ctrl-Space produces a space, not a NUL.
 *
 * The Ctrl column for the digit row and for the punctuation beyond
 * 0x0C/0x1A is not confirmed for an XT from that source: its control
 * entries there are PS/2 extended codes, and where it is silent this
 * table says "no character". That is recorded in the report rather than
 * guessed at, because a plausible wrong control code is exactly the kind
 * of thing that never gets found.
 */
static const struct key_def keymap[0x80] = {
    /* --- the main keyboard ---------------------------------------- */
    [0x01] = { K_CHAR,   0, 0,    0x1B, 0x1B, 0x1B },  /* Esc          */
    [0x02] = { K_CHAR,   0, 0,    '1',  '!',  0    },
    [0x03] = { K_CHAR,   0, 0,    '2',  '@',  0    },
    [0x04] = { K_CHAR,   0, 0,    '3',  '#',  0    },
    [0x05] = { K_CHAR,   0, 0,    '4',  '$',  0    },
    [0x06] = { K_CHAR,   0, 0,    '5',  '%',  0    },
    [0x07] = { K_CHAR,   0, 0,    '6',  '^',  0x1E },  /* Ctrl-6 = RS  */
    [0x08] = { K_CHAR,   0, 0,    '7',  '&',  0    },
    [0x09] = { K_CHAR,   0, 0,    '8',  '*',  0    },
    [0x0A] = { K_CHAR,   0, 0,    '9',  '(',  0    },
    [0x0B] = { K_CHAR,   0, 0,    '0',  ')',  0    },
    [0x0C] = { K_CHAR,   0, 0,    '-',  '_',  0x1F },  /* Ctrl-  = US  */
    [0x0D] = { K_CHAR,   0, 0,    '=',  '+',  0    },
    [0x0E] = { K_CHAR,   0, 0,    0x08, 0x08, 0x7F },  /* Backspace    */
    /* Tab sends 0x09 on its own and nothing at all under Shift or Ctrl.
     * There is no back-tab character to send: the published table's
     * shifted column for this key is 0x00, not another Tab. */
    [0x0F] = { K_CHAR,   0, 0,    0x09, 0x00, 0x00 },  /* Tab          */
    [0x10] = { K_LETTER, 0, 0,    'q',  'Q',  0x11 },
    [0x11] = { K_LETTER, 0, 0,    'w',  'W',  0x17 },
    [0x12] = { K_LETTER, 0, 0,    'e',  'E',  0x05 },
    [0x13] = { K_LETTER, 0, 0,    'r',  'R',  0x12 },
    [0x14] = { K_LETTER, 0, 0,    't',  'T',  0x14 },
    [0x15] = { K_LETTER, 0, 0,    'y',  'Y',  0x19 },
    [0x16] = { K_LETTER, 0, 0,    'u',  'U',  0x15 },
    [0x17] = { K_LETTER, 0, 0,    'i',  'I',  0x09 },
    [0x18] = { K_LETTER, 0, 0,    'o',  'O',  0x0F },
    [0x19] = { K_LETTER, 0, 0,    'p',  'P',  0x10 },
    [0x1A] = { K_CHAR,   0, 0,    '[',  '{',  0x1B },
    [0x1B] = { K_CHAR,   0, 0,    ']',  '}',  0x1D },
    [0x1C] = { K_CHAR,   0, 0,    0x0D, 0x0D, 0x0A },  /* Enter        */
    [0x1D] = { K_SHIFT,  BIOS16_HOLD_CTRL, 0, 0, 0, 0 },
    [0x1E] = { K_LETTER, 0, 0,    'a',  'A',  0x01 },
    [0x1F] = { K_LETTER, 0, 0,    's',  'S',  0x13 },
    [0x20] = { K_LETTER, 0, 0,    'd',  'D',  0x04 },
    [0x21] = { K_LETTER, 0, 0,    'f',  'F',  0x06 },
    [0x22] = { K_LETTER, 0, 0,    'g',  'G',  0x07 },
    [0x23] = { K_LETTER, 0, 0,    'h',  'H',  0x08 },
    [0x24] = { K_LETTER, 0, 0,    'j',  'J',  0x0A },
    [0x25] = { K_LETTER, 0, 0,    'k',  'K',  0x0B },
    [0x26] = { K_LETTER, 0, 0,    'l',  'L',  0x0C },
    [0x27] = { K_CHAR,   0, 0,    ';',  ':',  0    },
    [0x28] = { K_CHAR,   0, 0,    '\'', '"',  0    },
    [0x29] = { K_CHAR,   0, 0,    '`',  '~',  0    },
    [0x2A] = { K_SHIFT,  BIOS16_HOLD_LEFT_SHIFT, 0, 0, 0, 0 },
    [0x2B] = { K_CHAR,   0, 0,    '\\', '|',  0x1C },
    [0x2C] = { K_LETTER, 0, 0,    'z',  'Z',  0x1A },
    [0x2D] = { K_LETTER, 0, 0,    'x',  'X',  0x18 },
    [0x2E] = { K_LETTER, 0, 0,    'c',  'C',  0x03 },
    [0x2F] = { K_LETTER, 0, 0,    'v',  'V',  0x16 },
    [0x30] = { K_LETTER, 0, 0,    'b',  'B',  0x02 },
    [0x31] = { K_LETTER, 0, 0,    'n',  'N',  0x0E },
    [0x32] = { K_LETTER, 0, 0,    'm',  'M',  0x0D },
    [0x33] = { K_CHAR,   0, 0,    ',',  '<',  0    },
    [0x34] = { K_CHAR,   0, 0,    '.',  '>',  0    },
    [0x35] = { K_CHAR,   0, 0,    '/',  '?',  0    },
    [0x36] = { K_SHIFT,  BIOS16_HOLD_RIGHT_SHIFT, 0, 0, 0, 0 },
    /* The keypad's star doubles as Print Screen on a keyboard with a
     * Ctrl that reaches it; this module sends the character either way. */
    [0x37] = { K_CHAR,   0, 0,    '*',  '*',  '*'  },
    [0x38] = { K_SHIFT,  BIOS16_HOLD_ALT, 0, 0, 0, 0 },
    /* Every column of the space key is 0x20 in the published table,
     * Ctrl included. The folklore that Ctrl-Space is a NUL is not in it;
     * a key that produced no character there would be indistinguishable
     * from an arrow key, which is a much worse failure than a space. */
    [0x39] = { K_CHAR,   0, 0,    ' ',  ' ',  ' '  },
    [0x3A] = { K_LOCK,   0, BIOS16_LOCK_CAPS,   0, 0, 0 },
    [0x3B] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F1           */
    [0x3C] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F2           */
    [0x3D] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F3           */
    [0x3E] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F4           */
    [0x3F] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F5           */
    [0x40] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F6           */
    [0x41] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F7           */
    [0x42] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F8           */
    [0x43] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F9           */
    [0x44] = { K_CHAR,   0, 0,    0,    0,    0    },  /* F10          */
    [0x45] = { K_LOCK,   0, BIOS16_LOCK_NUM,    0, 0, 0 },
    [0x46] = { K_LOCK,   0, BIOS16_LOCK_SCROLL, 0, 0, 0 },

    /* --- the numeric keypad --------------------------------------- */
    /*
     * These eleven keys are labelled with a digit and a cursor function,
     * and which one they send depends on Num Lock -- which acts on them
     * exactly as Shift does. The base column is therefore the cursor
     * function, and on this part it is "no character at all": the
     * firmware has no ASCII for Up or Home and sends the scan code with
     * a zero in the character position, which is how a program tells an
     * arrow key from a digit.
     *
     * Num Lock is off in the state a machine comes up in here, so the
     * arrows are arrows until a program turns it on.
     */
    [0x47] = { K_KEYPAD, 0, 0,    0,   '7',  0    },  /* Home    / 7  */
    [0x48] = { K_KEYPAD, 0, 0,    0,   '8',  0    },  /* Up      / 8  */
    [0x49] = { K_KEYPAD, 0, 0,    0,   '9',  0    },  /* PgUp    / 9  */
    [0x4A] = { K_CHAR,   0, 0,    '-', '-',  '-'  },
    [0x4B] = { K_KEYPAD, 0, 0,    0,   '4',  0    },  /* Left    / 4  */
    [0x4C] = { K_KEYPAD, 0, 0,    0,   '5',  0    },  /* keypad  / 5  */
    [0x4D] = { K_KEYPAD, 0, 0,    0,   '6',  0    },  /* Right   / 6  */
    [0x4E] = { K_CHAR,   0, 0,    '+', '+',  '+'  },
    [0x4F] = { K_KEYPAD, 0, 0,    0,   '1',  0    },  /* End     / 1  */
    [0x50] = { K_KEYPAD, 0, 0,    0,   '2',  0    },  /* Down    / 2  */
    [0x51] = { K_KEYPAD, 0, 0,    0,   '3',  0    },  /* PgDn    / 3  */
    /* Insert is both a lock key and a cursor key: it flips the state a
     * program reads at 0040:0017 bit 7, and still reports itself. */
    [0x52] = { K_KEYPAD, 0, BIOS16_LOCK_INS, 0, '0', 0 },  /* Ins / 0 */
    [0x53] = { K_KEYPAD, 0, 0,    0,   '.',  0    },  /* Del     / .  */
};

/*
 * The character a key produces in the current state.
 *
 * A zero means "no character", and that is exactly how the firmware's
 * own table works: the extended keys store a zero in the character half
 * and leave the scan code as the answer. It has the side effect that
 * Ctrl-Space and Ctrl-2, which really do mean character zero, are
 * indistinguishable from an arrow key -- and on the hardware they are
 * indistinguishable too, so nothing is lost by not telling them apart.
 *
 * Ctrl is checked before Alt, which is what the firmware does. Alt is
 * checked before Shift, and produces nothing: the firmware's Alt column
 * is zero for every key, so Alt plus a letter is "the scan code, no
 * character", and a program that wants the Alt state reads it from
 * 0040:0017.
 */
static uint8_t key_translate(const struct key_def *key, uint8_t held,
                             uint8_t locks)
{
    bool shift = (held & (BIOS16_HOLD_LEFT_SHIFT | BIOS16_HOLD_RIGHT_SHIFT))
                 != 0;

    if (held & BIOS16_HOLD_CTRL)
        return key->control;

    if (held & BIOS16_HOLD_ALT)
        return 0;

    switch (key->kind) {
    case K_LETTER:
        /* Caps Lock is Shift for letters and for nothing else, and
         * holding Shift with Caps Lock on gives the lower case back. */
        return (shift != ((locks & BIOS16_LOCK_CAPS) != 0)) ? key->shift
                                                            : key->base;
    case K_KEYPAD:
        return (shift != ((locks & BIOS16_LOCK_NUM) != 0)) ? key->shift
                                                           : key->base;
    case K_CHAR:
        return shift ? key->shift : key->base;

    case K_NONE:
    case K_SHIFT:
    case K_LOCK:
        break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* The hardware side                                                   */
/* ------------------------------------------------------------------ */

void bios16_key_arrived(struct bios16_state *st, uint8_t scancode)
{
    /*
     * A one-byte register with no queue behind it. A second key arriving
     * before INT 09h has read the first overwrites it, which is what the
     * controller does -- and it is a real situation, not a theoretical
     * one: a guest that has replaced INT 09h and does not read port 60h
     * will drop keys, and that is its own doing.
     */
    st->controller_output = scancode;
    st->controller_full   = true;
}

void bios16_irq(struct vm86_cpu *cpu, void *ctx)
{
    struct bios16_state *st = ctx;
    uint8_t code, make;
    bool    released;
    const struct key_def *key;

    if (!st->controller_full)
        return;   /* an interrupt with no key behind it is not an error */

    code = st->controller_output;
    st->controller_full = false;

    released = (code & 0x80u) != 0;
    make     = (uint8_t)(code & 0x7Fu);
    key      = &keymap[make];

    switch (key->kind) {
    case K_NONE:
        return;

    case K_SHIFT:
        if (released)
            st->held &= (uint8_t)~key->hold;
        else
            st->held |= key->hold;

        kb_write_shift_flags(cpu, st);
        return;

    case K_LOCK:
        /* Only on the way down. The break code for Caps Lock is 0xBA and
         * it must not undo what the make code just did. */
        if (!released)
            kb_toggle_flag(cpu, key->toggle);
        return;

    case K_CHAR:
    case K_LETTER:
    case K_KEYPAD:
        break;
    }

    if (released)
        return;   /* a character key coming back up produces nothing */

    /* Insert is a lock key and a cursor key at once. */
    if (key->toggle)
        kb_toggle_flag(cpu, key->toggle);

    uint8_t ascii = key_translate(key, st->held, kb_read_flags(cpu));

    kb_push(cpu, (uint16_t)(((uint16_t)make << 8) | ascii));
}

/* ------------------------------------------------------------------ */
/* The software side                                                   */
/* ------------------------------------------------------------------ */

/*
 * AH=00h, read a key, does not return until there is one.
 *
 * On the hardware this is a spin with interrupts enabled, so that the
 * keyboard interrupt can run between two trips around the loop and fill
 * the buffer. That cannot be written as a spin here: this service is
 * called from inside the interpreter, on one thread, and a service that
 * does not return is a host that never gets control back -- so the key
 * it is waiting for can never be delivered.
 *
 * `vm86_service_retry()` is the way out. It undoes the interrupt that
 * reached this service -- pops the frame the guest's INT pushed, rewinds
 * the instruction pointer to the INT itself -- and returns. The run loop
 * finishes its slice, the host feeds a key and raises IRQ1, and the guest
 * re-executes the INT and finds one. The waiting happens outside, where
 * it belongs.
 *
 * It is a retry and not a re-entrant wait, so it costs nothing to be
 * wrong about when to use it -- except that using it with a key already
 * in the buffer would spin forever without ever consuming one, and the
 * only symptom would be a program that stops moving. Hence the ordering
 * below: take the key first, and retry only when there was none.
 */
void bios16_service(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;   /* the queue lives in guest memory, not in the state */

    uint16_t word;

    switch (cpu->ah) {
    case 0x00:
        if (!kb_take(cpu, &word)) {
            /*
             * Ask to be run again rather than answer with nothing. This
             * is the only shape a wait can take here: the interpreter is
             * one thread, so a service that spins never returns and the
             * key it waits for never arrives. Re-running the trap puts
             * the wait outside, where the run loop still gets a look in
             * and can deliver the keyboard interrupt.
             *
             * Interrupts are not turned on here even though the wait
             * needs them and the INT that got us here turned them off.
             * vm86_service_retry() does it, along with the reasoning.
             *
             * That is a change from how this service was first written,
             * which set IF itself. It worked, and it was one line in the
             * wrong place: every blocking service would have had to
             * remember it, and forgetting is not a compile error or a
             * failed test -- it is a machine that stops moving. The
             * service that forgets is the one that never gets written
             * against a test that waits.
             */
            vm86_service_retry(cpu);
            return;
        }

        cpu->al = (uint8_t)(word & 0x00FFu);
        cpu->ah = (uint8_t)(word >> 8);
        return;

    case 0x01:
        /*
         * A look, not a take. The caller decides whether to come back for
         * the key, and consuming it here would lose it -- programs check
         * with this and then read with 00h, and the second call would
         * find nothing.
         */
        if (kb_peek(cpu, &word)) {
            cpu->al = (uint8_t)(word & 0x00FFu);
            cpu->ah = (uint8_t)(word >> 8);
            vm86_flag_set(cpu, VM86_ZF, false);
        } else {
            /*
             * Nothing waiting. ZF is the answer, and AX is deliberately
             * left alone: the firmware does not define it here, and
             * writing a zero would be inventing a keystroke with scan
             * code zero and character zero, which is what a program that
             * ignores ZF would then act on.
             */
            vm86_flag_set(cpu, VM86_ZF, true);
        }
        return;

    case 0x02:
        /*
         * AL and nothing else.
         *
         * This one reports the shift state, not a keystroke: there is no
         * scan code to hand back and no character either, and a caller
         * that catches this function answering AH=0 has been told
         * something false about a key that does not exist.
         *
         * The task book originally asked for the scan code in AH, with
         * zero when there was none, and that was wrong -- it was the
         * 00h/01h answer written one row too far down the table. See
         * dos-refs.md section 5, which also records that the published
         * tables disagree about AL against AH and that the detailed
         * sources are unanimous for AL.
         */
        cpu->al = kb_read_flags(cpu);
        return;

    default:
        /* A function number this firmware does not implement. Falling
         * through to the stub's IRET with the registers untouched is the
         * honest answer: AH keeps the value the caller put there, which
         * is how a program detects that the call did nothing. */
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void bios16_reset(struct bios16_state *st)
{
    st->controller_output = 0;
    st->controller_full   = false;
    st->held              = 0;
}

void bios16_init(struct vm86_cpu *cpu, struct bios16_state *st)
{
    bios16_reset(st);

    vm86_mem_write8(cpu->mem, bda_linear(VM86_BDA_KEYBOARD_FLAGS), 0);
    vm86_mem_write8(cpu->mem, bda_linear(VM86_BDA_KEYBOARD_FLAGS2), 0);

    /* An empty ring is head == tail == 0x1E. Zero would be a buffer
     * whose pointers address the middle of the data area. */
    vm86_mem_write16(cpu->mem, bda_linear(VM86_BDA_KB_HEAD),
                     (uint16_t)VM86_BDA_KB_EMPTY);
    vm86_mem_write16(cpu->mem, bda_linear(VM86_BDA_KB_TAIL),
                     (uint16_t)VM86_BDA_KB_EMPTY);
}
