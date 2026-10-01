#include <funnyos/kbd.h>

#include <funnyos/arch/x86_64/apic.h>
#include <funnyos/arch/x86_64/cpu.h>
#include <funnyos/arch/x86_64/io.h>
#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/kprintf.h>

#include <stddef.h>

/* --- 8042 controller ----------------------------------------------- */

#define KBD_DATA    0x60
#define KBD_STATUS  0x64   /* read */
#define KBD_COMMAND 0x64   /* write */

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_AUX_DATA    0x20   /* the byte came from the mouse port */

#define CMD_READ_CONFIG  0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_DISABLE_KBD  0xAD
#define CMD_ENABLE_KBD   0xAE

/* A bounded wait, because a controller that never answers must not become
 * a boot that never finishes. Each iteration is one port read. */
#define KBD_TIMEOUT 100000u

static bool wait_input_clear(void)
{
    for (uint32_t i = 0; i < KBD_TIMEOUT; i++)
        if (!(inb(KBD_STATUS) & STATUS_INPUT_FULL))
            return true;
    return false;
}

static bool wait_output_full(void)
{
    for (uint32_t i = 0; i < KBD_TIMEOUT; i++)
        if (inb(KBD_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    return false;
}

static bool controller_command(uint8_t command)
{
    if (!wait_input_clear())
        return false;
    outb(KBD_COMMAND, command);
    return true;
}

static bool controller_data_out(uint8_t value)
{
    if (!wait_input_clear())
        return false;
    outb(KBD_DATA, value);
    return true;
}

static bool controller_data_in(uint8_t *out)
{
    if (!wait_output_full())
        return false;
    *out = inb(KBD_DATA);
    return true;
}

static void controller_flush(void)
{
    for (int i = 0; i < 32; i++) {
        if (!(inb(KBD_STATUS) & STATUS_OUTPUT_FULL))
            break;
        (void)inb(KBD_DATA);
    }
}

/* --- Scancode tables ----------------------------------------------- */

/*
 * Scancode set 1, index = scancode with the release bit stripped.
 *
 * Modifier keys hold zero here: they are handled by the decoder before
 * the table is consulted, and a zero in the table would otherwise be
 * indistinguishable from "this key has no character".
 */
static const char g_normal[128] = {
/* 00 */   0,            KEY_ESCAPE,   '1',  '2',  '3',  '4',  '5',  '6',
/* 08 */   '7',          '8',          '9',  '0',  '-',  '=',  KEY_BACKSPACE, KEY_TAB,
/* 10 */   'q',          'w',          'e',  'r',  't',  'y',  'u',  'i',
/* 18 */   'o',          'p',          '[',  ']',  KEY_ENTER, 0,  'a',  's',
/* 20 */   'd',          'f',          'g',  'h',  'j',  'k',  'l',  ';',
/* 28 */   '\'',         '`',           0,   '\\', 'z',  'x',  'c',  'v',
/* 30 */   'b',          'n',          'm',  ',',  '.',  '/',   0,   '*',
/* 38 */   0,            ' ',           0,    0,    0,    0,    0,    0,
/* 40 */   0,            0,             0,    0,    0,    0,    0,   '7',
/* 48 */   '8',          '9',          '-',  '4',  '5',  '6',  '+',  '1',
/* 50 */   '2',          '3',          '0',  '.',   0,    0,    0,    0,
/* 58 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 60 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 68 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 70 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 78 */   0,            0,             0,    0,    0,    0,    0,    0,
};

static const char g_shifted[128] = {
/* 00 */   0,            KEY_ESCAPE,   '!',  '@',  '#',  '$',  '%',  '^',
/* 08 */   '&',          '*',          '(',  ')',  '_',  '+',  KEY_BACKSPACE, KEY_TAB,
/* 10 */   'Q',          'W',          'E',  'R',  'T',  'Y',  'U',  'I',
/* 18 */   'O',          'P',          '{',  '}',  KEY_ENTER, 0,  'A',  'S',
/* 20 */   'D',          'F',          'G',  'H',  'J',  'K',  'L',  ':',
/* 28 */   '"',          '~',           0,   '|',  'Z',  'X',  'C',  'V',
/* 30 */   'B',          'N',          'M',  '<',  '>',  '?',   0,   '*',
/* 38 */   0,            ' ',           0,    0,    0,    0,    0,    0,
/* 40 */   0,            0,             0,    0,    0,    0,    0,   '7',
/* 48 */   '8',          '9',          '-',  '4',  '5',  '6',  '+',  '1',
/* 50 */   '2',          '3',          '0',  '.',   0,    0,    0,    0,
/* 58 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 60 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 68 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 70 */   0,            0,             0,    0,    0,    0,    0,    0,
/* 78 */   0,            0,             0,    0,    0,    0,    0,    0,
};

/* --- Decoder ------------------------------------------------------- */

int kbd_decode(struct kbd_state *state, uint8_t scancode)
{
    /*
     * Multi-byte sequences are consumed before anything else is
     * interpreted. This has to come first, because the Pause sequence
     * contains a second 0xE1 partway through: if the prefix test ran
     * before this one, that inner 0xE1 would re-arm the counter and the
     * sequence would leave the decoder swallowing several real
     * keystrokes afterwards.
     */
    if (state->swallow) {
        state->swallow--;
        return KEY_NONE;
    }

    /* 0xE0 prefixes one byte, turning it into one of the keys the
     * original PC keyboard did not have. */
    if (scancode == 0xE0) {
        state->extended = true;
        return KEY_NONE;
    }

    /*
     * 0xE1 begins Pause, a six-byte sequence. Its second byte is 0x1D,
     * which the decoder would otherwise read as left control -- so Pause
     * would quietly toggle ctrl and every following key would be wrong
     * until it was pressed again. Swallowing the remaining bytes is the
     * whole fix.
     */
    if (scancode == 0xE1) {
        state->extended = false;
        state->swallow  = 5;
        return KEY_NONE;
    }

    bool    release = (scancode & 0x80) != 0;
    uint8_t code    = scancode & 0x7F;

    if (state->extended) {
        state->extended = false;

        switch (code) {
        case 0x1D: state->ctrl = !release; return KEY_NONE;   /* right ctrl */
        case 0x38: state->alt  = !release; return KEY_NONE;   /* right alt */
        case 0x48: return release ? KEY_NONE : KEY_UP;
        case 0x50: return release ? KEY_NONE : KEY_DOWN;
        case 0x4B: return release ? KEY_NONE : KEY_LEFT;
        case 0x4D: return release ? KEY_NONE : KEY_RIGHT;
        case 0x47: return release ? KEY_NONE : KEY_HOME;
        case 0x4F: return release ? KEY_NONE : KEY_END;
        case 0x49: return release ? KEY_NONE : KEY_PAGE_UP;
        case 0x51: return release ? KEY_NONE : KEY_PAGE_DOWN;
        case 0x52: return release ? KEY_NONE : KEY_INSERT;
        case 0x53: return release ? KEY_NONE : KEY_DELETE;
        case 0x1C: return release ? KEY_NONE : KEY_ENTER;      /* keypad enter */
        default:   return KEY_NONE;
        }
    }

    switch (code) {
    case 0x2A: state->shift = !release; return KEY_NONE;
    case 0x36: state->shift = !release; return KEY_NONE;
    case 0x1D: state->ctrl  = !release; return KEY_NONE;
    case 0x38: state->alt   = !release; return KEY_NONE;
    case 0x3A:
        /* Caps lock toggles on press. The release is ignored, or the
         * toggle would happen twice per keystroke and nothing would
         * ever change. */
        if (!release)
            state->caps = !state->caps;
        return KEY_NONE;
    default:
        break;
    }

    if (release)
        return KEY_NONE;

    char lower = g_normal[code];
    if (lower == 0)
        return KEY_NONE;

    char upper = g_shifted[code];

    /* Caps lock applies to letters only, and shift cancels it: caps and
     * shift together give lowercase, which is what every keyboard does
     * and the opposite of what "shift means uppercase" would suggest. */
    bool alpha = (lower >= 'a' && lower <= 'z');
    char out   = alpha
               ? ((state->shift != state->caps) ? upper : lower)
               : (state->shift ? upper : lower);

    return (int)(unsigned char)out;
}

/* --- Key queue ----------------------------------------------------- */

#define KBD_QUEUE_SIZE 128

static volatile int      g_queue[KBD_QUEUE_SIZE];
static volatile uint32_t g_head;
static volatile uint32_t g_tail;

static struct kbd_state g_state;
static uint64_t         g_scancodes;
static bool             g_ready;
static const struct process *g_raw_owner;
static volatile uint8_t g_raw[512];
static volatile unsigned g_raw_head, g_raw_tail;
static volatile bool g_raw_overflow;

bool kbd_raw_acquire(const struct process *who)
{
    bool enabled=interrupts_enabled(); interrupts_disable();
    bool ok=who && (!g_raw_owner || g_raw_owner==who);
    if(ok && !g_raw_owner) {
        g_raw_owner=who; g_raw_head=g_raw_tail=0; g_raw_overflow=false;
        g_tail=g_head;
    }
    if(enabled) interrupts_enable();
    return ok;
}
int kbd_raw_poll(const struct process *who)
{
    if(!who || g_raw_owner!=who) return -1;
    if(g_raw_overflow) return -10;
    if(g_raw_head==g_raw_tail) return -1;
    int byte=g_raw[g_raw_tail]; g_raw_tail=(g_raw_tail+1)%512; return byte;
}
bool kbd_raw_release(const struct process *who)
{
    bool enabled=interrupts_enabled(); interrupts_disable();
    bool ok=who && g_raw_owner==who;
    if(ok) {
        g_raw_owner=NULL; g_raw_head=g_raw_tail=0; g_raw_overflow=false;
        g_tail=g_head; g_state=(struct kbd_state){0};
    }
    if(enabled) interrupts_enable();
    return ok;
}
void kbd_raw_release_if_held_by(const struct process *who) { (void)kbd_raw_release(who); }


static void queue_push(int key)
{
    uint32_t next = (g_head + 1) % KBD_QUEUE_SIZE;

    /* Full. Drop the newest keystroke rather than overwriting the oldest:
     * the oldest is the one that was typed first, and losing the end of a
     * typed word is less confusing than losing its beginning. */
    if (next == g_tail)
        return;

    g_queue[g_head] = key;
    g_head = next;
}

int kbd_poll(void)
{
    if (g_tail == g_head)
        return KEY_NONE;

    int key = g_queue[g_tail];
    g_tail = (g_tail + 1) % KBD_QUEUE_SIZE;
    return key;
}

int kbd_getchar(void)
{
    for (;;) {
        int key = kbd_poll();
        if (key != KEY_NONE)
            return key;

        /* With interrupts disabled, hlt never returns. Reporting "no key"
         * instead of hanging keeps a mistake in the boot order from
         * looking like a dead machine. */
        if (!interrupts_enabled())
            return KEY_NONE;

        cpu_halt();
    }
}

/* --- Interrupt handler --------------------------------------------- */

static void kbd_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;

    /*
     * Drain the controller rather than reading one byte.
     *
     * A fast typist, a key repeat, or a stalled CPU can leave several
     * scancodes queued, and the 8042 raises the interrupt on the
     * transition to non-empty -- so a byte left unread may never produce
     * another interrupt and the key is simply lost.
     */
    for (;;) {
        uint8_t status = inb(KBD_STATUS);
        if (!(status & STATUS_OUTPUT_FULL))
            break;

        uint8_t data = inb(KBD_DATA);

        /* Byte came from the mouse port. There is no mouse driver, and
         * feeding its packets to the scancode decoder types garbage. */
        if (status & STATUS_AUX_DATA)
            continue;

        g_scancodes++;

        if(g_raw_owner) {
            unsigned next=(g_raw_head+1)%512;
            if(next==g_raw_tail) g_raw_overflow=true;
            else { g_raw[g_raw_head]=data; g_raw_head=next; }
        } else {
            int key = kbd_decode(&g_state, data);
            if (key != KEY_NONE) queue_push(key);
        }
    }
}

/* --- Setup --------------------------------------------------------- */

bool kbd_init(void)
{
    controller_flush();

    /* Take the keyboard offline while its configuration is rewritten, so
     * a keystroke cannot interleave with the change. */
    if (!controller_command(CMD_DISABLE_KBD))
        return false;

    controller_flush();

    if (!controller_command(CMD_READ_CONFIG))
        return false;

    uint8_t config;
    if (!controller_data_in(&config))
        return false;

    config |= 0x01;    /* enable IRQ 1 */
    config &= ~0x10;   /* enable the keyboard clock */

    /* Our public raw stream is set 1, not whatever firmware happened to
     * leave selected. The 8042 translates the keyboard's set 2 stream. */
    config |= 0x40;

    if (!controller_command(CMD_WRITE_CONFIG))
        return false;
    if (!controller_data_out(config))
        return false;

    if (!controller_command(CMD_ENABLE_KBD))
        return false;

    controller_flush();

    if (!irq_register(IRQ_VECTOR_KEYBOARD, kbd_irq, NULL)) {
        kprintf("kbd: vector %u is already claimed\n",
                (unsigned)IRQ_VECTOR_KEYBOARD);
        return false;
    }

    ioapic_route_isa(KBD_ISA_IRQ, IRQ_VECTOR_KEYBOARD);

    g_ready = true;
    return true;
}

bool kbd_ready(void)              { return g_ready; }
uint64_t kbd_scancode_count(void) { return g_scancodes; }

/* --- Self-test ----------------------------------------------------- */

struct kbd_case {
    const char *what;
    uint8_t     bytes[8];
    uint8_t     count;
    int         expect[4];
    uint8_t     expect_count;
};

/*
 * Each case is fed through a fresh decoder, and the keys that come out
 * must match the expected sequence exactly -- same keys, same order, same
 * count.
 *
 * These cover the parts that are easy to get subtly wrong and impossible
 * to notice by using the machine: shift cancelling caps lock, a release
 * not producing a second keystroke, a prefix applying to exactly one
 * byte, and a multi-byte sequence that has to be swallowed whole.
 */
static const struct kbd_case g_cases[] = {
    { "a",                 { 0x1E },                           1, { 'a' },            1 },
    { "a released",        { 0x1E, 0x9E },                     2, { 'a' },            1 },
    { "shift+a",           { 0x2A, 0x1E },                     2, { 'A' },            1 },
    { "shift released",    { 0x2A, 0xAA, 0x1E },               3, { 'a' },            1 },
    { "1",                 { 0x02 },                           1, { '1' },            1 },
    { "shift+1",           { 0x2A, 0x02 },                     2, { '!' },            1 },
    { "caps+a",            { 0x3A, 0x1E },                     2, { 'A' },            1 },
    { "caps+shift+a",      { 0x3A, 0x2A, 0x1E },               3, { 'a' },            1 },
    { "caps twice",        { 0x3A, 0x3A, 0x1E },               3, { 'a' },            1 },
    { "caps leaves 1 alone", { 0x3A, 0x02 },                   2, { '1' },            1 },
    { "enter",             { 0x1C },                           1, { KEY_ENTER },      1 },
    { "backspace",         { 0x0E },                           1, { KEY_BACKSPACE },  1 },
    { "space",             { 0x39 },                           1, { ' ' },            1 },
    { "minus",             { 0x0C },                           1, { '-' },            1 },
    { "shift+minus",       { 0x2A, 0x0C },                     2, { '_' },            1 },
    { "quote",             { 0x28 },                           1, { '\'' },           1 },
    { "shift+quote",       { 0x2A, 0x28 },                     2, { '"' },            1 },
    { "backslash",         { 0x2B },                           1, { '\\' },           1 },
    { "extended up",       { 0xE0, 0x48 },                     2, { KEY_UP },         1 },
    { "extended down",     { 0xE0, 0x50 },                     2, { KEY_DOWN },       1 },
    { "extended left",     { 0xE0, 0x4B },                     2, { KEY_LEFT },       1 },
    { "extended right",    { 0xE0, 0x4D },                     2, { KEY_RIGHT },      1 },
    { "extended up release", { 0xE0, 0xC8 },                   2, { },               0 },
    { "prefix spans one byte", { 0xE0, 0x48, 0x1E },           3, { KEY_UP, 'a' },    2 },
    { "shift spans its keys", { 0x2A, 0x1E, 0xAA, 0x1E },      4, { 'A', 'a' },       2 },
    { "repeats",           { 0x1E, 0x1E, 0x1E },               3, { 'a', 'a', 'a' },  3 },
    { "ctrl is not a key", { 0x1D, 0x1E },                     2, { 'a' },            1 },
    /* Pause is six bytes; the seventh is a real key that must still work
     * afterwards, which is what catches the counter being re-armed. */
    { "pause is swallowed", { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5, 0x1E },
                                                               7, { 'a' },            1 },
};

#define KBD_CASE_COUNT (sizeof(g_cases) / sizeof(g_cases[0]))

bool kbd_selftest(void)
{
    for (size_t i = 0; i < KBD_CASE_COUNT; i++) {
        struct kbd_state state = { false, 0, false, false, false, false };
        int     got[4] = { 0, 0, 0, 0 };
        uint8_t got_count = 0;
        bool    overflow  = false;

        for (uint8_t b = 0; b < g_cases[i].count; b++) {
            int key = kbd_decode(&state, g_cases[i].bytes[b]);
            if (key == KEY_NONE)
                continue;
            if (got_count < 4)
                got[got_count++] = key;
            else
                overflow = true;
        }

        bool ok = !overflow && got_count == g_cases[i].expect_count;
        for (uint8_t k = 0; ok && k < got_count; k++)
            ok = (got[k] == g_cases[i].expect[k]);

        if (!ok) {
            kprintf("      case '%s': expected %u key(s), got %u%s\n",
                    g_cases[i].what, (unsigned)g_cases[i].expect_count,
                    (unsigned)got_count, overflow ? " and more" : "");
            kprintf("        expected:");
            for (uint8_t k = 0; k < g_cases[i].expect_count; k++)
                kprintf(" %d", g_cases[i].expect[k]);
            kprintf("\n        got     :");
            for (uint8_t k = 0; k < got_count; k++)
                kprintf(" %d", got[k]);
            kprintf("\n");
            return false;
        }
    }

    return true;
}
