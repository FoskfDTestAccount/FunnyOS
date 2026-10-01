/*
 * PS/2 keyboard.
 *
 * The controller is an 8042 with two ports -- keyboard on 0x60, status and
 * command on 0x64 -- and it is a legacy device in the truest sense: the
 * status register has to be read before the data register, an interrupt is
 * raised per byte rather than per keystroke, and the scancodes that arrive
 * are not the ones the keyboard sends.
 *
 * That last point is worth stating plainly. Modern keyboards speak
 * scancode set 2. The 8042 has a translation bit that rewrites set 2 into
 * set 1 before the CPU ever sees it, and firmware leaves it on. So this
 * driver decodes set 1, and if a machine ever shows up with translation
 * off, every key would be wrong rather than merely missing -- which is the
 * kind of failure worth knowing to look for.
 *
 * Interrupts arrive on ISA IRQ 1, routed through the IO APIC. The handler
 * drains the controller rather than reading one byte: keystrokes queue up
 * faster than interrupt latency, and a byte left in the output buffer is a
 * byte that may never generate another interrupt.
 */
#ifndef FUNNYOS_KBD_H
#define FUNNYOS_KBD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ISA IRQ the keyboard is wired to. */
#define KBD_ISA_IRQ 1

/*
 * Key codes. Printable characters come back as themselves, so the values
 * below 0x100 are exactly the bytes a program would expect from a
 * terminal. Anything above is a key with no character of its own; the
 * value is deliberately outside the byte range so a caller cannot mistake
 * one for a character.
 */
#define KEY_NONE        0
#define KEY_ESCAPE      0x1B
#define KEY_BACKSPACE   '\b'
#define KEY_TAB         '\t'
#define KEY_ENTER       '\n'

#define KEY_UP          0x101
#define KEY_DOWN        0x102
#define KEY_LEFT        0x103
#define KEY_RIGHT       0x104
#define KEY_HOME        0x105
#define KEY_END         0x106
#define KEY_DELETE      0x107
#define KEY_INSERT      0x108
#define KEY_PAGE_UP     0x109
#define KEY_PAGE_DOWN   0x10A
#define KEY_F1          0x110   /* F1..F12 occupy 0x110..0x11B */

/*
 * Decoder state, kept in a struct rather than in file scope so the
 * translation tables can be exercised against a fresh instance without
 * disturbing whatever the real keyboard is in the middle of typing.
 */
struct kbd_state {
    bool    extended;    /* last byte was the 0xE0 prefix */
    uint8_t swallow;     /* bytes still owed to a multi-byte sequence */
    bool    shift;
    bool    ctrl;
    bool    alt;
    bool    caps;
};

/* Bring up the controller, route IRQ 1 and register the handler. */
bool kbd_init(void);

bool kbd_ready(void);

/* Decode one scancode. Returns KEY_NONE when the byte changes modifier
 * state or is part of a sequence with no key of its own. */
int kbd_decode(struct kbd_state *state, uint8_t scancode);

/* --- Key queue ------------------------------------------------------ */

/* Next key, or KEY_NONE when nothing is waiting. */
int kbd_poll(void);

/* Next key, blocking with hlt until one arrives. The only way this
 * returns KEY_NONE is if interrupts are disabled, in which case it
 * returns immediately rather than hanging. */
int kbd_getchar(void);

/* --- Diagnostics ---------------------------------------------------- */

/* Verify the scancode translation against a table of known sequences.
 * Runs on every boot: a wrong key is otherwise only noticed by whoever is
 * typing, and only as "the keyboard is broken". */
bool kbd_selftest(void);

/* Scancodes decoded since boot, including modifier presses. */
uint64_t kbd_scancode_count(void);

struct process;
bool kbd_raw_acquire(const struct process *who);
int kbd_raw_poll(const struct process *who);
bool kbd_raw_release(const struct process *who);
void kbd_raw_release_if_held_by(const struct process *who);

#endif /* FUNNYOS_KBD_H */
