/*
 * INT 16h -- the keyboard.
 *
 * ---------------------------------------------------------------------
 * What this is
 *
 * A program asks for a key by executing INT 16h with a function number in
 * AH. The scan code tables, the ring buffer in the BIOS data area and the
 * shift flags at 0040:0017 are all published, and a program that reads
 * the buffer pointers directly is doing something ordinary rather than
 * something clever. This module is both halves of that: the service a
 * program calls, and the interrupt handler that fills the buffer the
 * service reads.
 *
 * ---------------------------------------------------------------------
 * Two things, not one
 *
 * A keyboard has a hardware side and a software side, and they are kept
 * apart here because a program is allowed to replace the software side.
 *
 * The hardware side is the keyboard controller: a scan code appears in
 * its output register and it asserts IRQ1. That register is
 * `controller_output`, and `bios16_key_arrived()` is how a host puts a
 * code into it. The host owns the physical keyboard; nothing in this
 * module ever invents a keystroke.
 *
 * The software side is INT 09h, which reads that register, translates the
 * code against the shift state, and pushes a word into 0040:001E. That
 * vector is a program's to point elsewhere -- a program that installs its
 * own INT 09h to watch for a hot key reads the register itself and never
 * puts anything in the buffer, and that is its right.
 *
 * Separating the register from the buffer is what makes that possible: a
 * guest that replaces INT 09h loses the translation but the machine can
 * still model a key having been pressed.
 *
 * ---------------------------------------------------------------------
 * Who owns what
 *
 * The BIOS data area fields for the keyboard -- the two shift-flag bytes
 * at 0x17 and 0x18, the ring pointers at 0x1A and 0x1C, and the ring
 * itself at 0x1E -- belong to this module and nothing else may write
 * them. A second writer is how two services would come to disagree about
 * the same fact.
 *
 * The module does not allocate. `struct bios16_state` is complete here so
 * that a caller can embed one; the guest's memory is the caller's, and so
 * is the machine the state describes. A test can therefore have two.
 */
#ifndef VM86_BIOS16_H
#define VM86_BIOS16_H

#include <stdbool.h>
#include <stdint.h>

#include <vm86/cpu.h>

/* ------------------------------------------------------------------ */
/* The hardware side                                                   */
/* ------------------------------------------------------------------ */

/*
 * Shift keys currently held down.
 *
 * The low four bits are deliberately the same bits they occupy in
 * 0040:0017, so that writing the shift-flag byte is an OR rather than a
 * translation. That correspondence is not a coincidence of the layout:
 * the byte was defined for exactly these four keys, and the rest of it
 * holds the lock keys, which are a different kind of thing -- they stay
 * set after the key comes back up, and so they cannot be derived from
 * what is held.
 */
#define BIOS16_HOLD_RIGHT_SHIFT 0x01u
#define BIOS16_HOLD_LEFT_SHIFT  0x02u
#define BIOS16_HOLD_CTRL        0x04u
#define BIOS16_HOLD_ALT         0x08u

struct bios16_state {
    /*
     * Port 60h: the last scan code the keyboard controller put on the
     * wire, which INT 09h has not read yet.
     *
     * `controller_full` is the "output buffer full" status bit. It
     * matters: a guest that has hooked INT 09h may leave the register
     * alone, and the next key then overwrites it -- which is what the
     * hardware does, since the controller holds one byte and no more.
     */
    uint8_t controller_output;
    bool    controller_full;

    /* Which shift keys are down, as BIOS16_HOLD_*. */
    uint8_t held;
};

/*
 * A scan code arrived from the host's keyboard.
 *
 * Puts it in the controller's output register. It does not translate it
 * and does not touch the buffer: that is INT 09h's job, and INT 09h may
 * not be ours. The caller raises IRQ1 afterwards, which is what a real
 * controller does when it loads the register.
 */
void bios16_key_arrived(struct bios16_state *st, uint8_t scancode);

/*
 * INT 09h -- the keyboard interrupt handler.
 *
 * Reads the controller's output register, translates it against the
 * current shift state, updates the shift flags, and pushes the resulting
 * ASCII/scan-code pair into the buffer at 0040:001E. When the buffer is
 * full the keystroke is discarded and the overflow is flagged, which is
 * what the firmware does and what keeps a program that never reads keys
 * from having its head pointer written over.
 *
 * Register this against VM86_INT_KEYBOARD, or call it directly from a
 * test. `ctx` is a `struct bios16_state *`.
 */
void bios16_irq(struct vm86_cpu *cpu, void *ctx);

/* ------------------------------------------------------------------ */
/* The software side                                                   */
/* ------------------------------------------------------------------ */

/*
 * INT 16h -- what a program calls.
 *
 *   AH=00h  read a key, waiting for one
 *   AH=01h  report whether one is waiting, without taking it
 *   AH=02h  report the shift state from 0040:0017
 *
 * Read the commentary on the AH=00h case in the .c file before changing
 * anything here: a blocking read is the one place in this milestone where
 * a service cannot simply compute an answer and return.
 *
 * `ctx` is a `struct bios16_state *`.
 */
void bios16_service(struct vm86_cpu *cpu, void *ctx);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * Forget the hardware state: the output register and which shift keys are
 * held. Does not touch guest memory -- the data area belongs to the
 * machine, not to this structure, and a caller that wants it initialised
 * wants `bios16_init()`.
 */
void bios16_reset(struct bios16_state *st);

/*
 * Reset the hardware state and initialise the data-area fields this
 * module owns: the two shift-flag bytes to zero, and the ring pointers to
 * point at the start of an empty buffer.
 *
 * The pointers are the reason this exists as a separate call. They are
 * relative to the segment and an empty ring is head == tail == 0x1E, not
 * zero -- a buffer left holding the zeroes guest memory starts with is a
 * buffer whose head and tail point into the middle of the data area, and
 * the first keystroke would be written over the equipment word.
 */
void bios16_init(struct vm86_cpu *cpu, struct bios16_state *st);

#endif /* VM86_BIOS16_H */
