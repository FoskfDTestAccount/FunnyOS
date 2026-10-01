/*
 * INT 1Ah -- the clock, and INT 08h which drives it.
 *
 * ---------------------------------------------------------------------
 * What the machine counts
 *
 * The 8253 timer is fed 1.193182 MHz and the firmware programs channel 0
 * to divide that by 65536, so it raises IRQ0 18.2065 times a second --
 * a tick every 54.925493 ms. That odd rate is not a design anyone chose
 * for its own sake: the divider is the largest one that fits in the
 * chip's sixteen-bit counter, and the input frequency is a third of the
 * colour burst crystal that happened to be on the board.
 *
 * The count itself lives at 0040:006C as a double word, and it counts
 * from midnight: at 0x1800B0 ticks -- one day -- it goes back to zero
 * and raises the flag at 0040:0070 bit 0. A program that reads the count
 * across that moment uses the flag to tell which side of midnight it is
 * on, and clears the flag itself.
 *
 * The flag is *set*, never added to. Two midnights with nobody reading
 * the count in between therefore lose one, and a program that keeps its
 * own calendar that way drifts. That is what the firmware does -- the
 * defect is in the machine being emulated, not in this file -- and it is
 * written down here so that the next reader does not find it, decide it
 * is a bug, and fix it into something that disagrees with every program
 * written against the real thing.
 *
 * ---------------------------------------------------------------------
 * Where the host's clock becomes the guest's
 *
 * The host's clock does not run at 18.2 Hz and has no reason to know that
 * number. Something has to convert, and where the conversion lives is a
 * decision:
 *
 *   - in the host, every host would carry the constant, and there are
 *     several: the host unit tests, the FunnyOS process, whatever the
 *     next one is;
 *   - here, the constant exists once and a host only ever says how many
 *     milliseconds have gone by.
 *
 * It lives here. `bios1a_advance()` takes milliseconds and returns how
 * many ticks those milliseconds are worth; the host raises that many
 * IRQ0s.
 *
 * ---------------------------------------------------------------------
 * Who increments the count
 *
 * The INT 08h handler does -- not `bios1a_advance()` and not the host.
 * That is what the firmware does, and it has a consequence a program can
 * observe: a program that hooks INT 08h and does not chain back to the
 * old handler stops the BIOS clock. It is not a bug in the emulator. It
 * is the machine.
 *
 * So `bios1a_advance()` answers "how many ticks are owed" and the 08h
 * handler is the only thing that adds to 0040:006C.
 *
 * A tick that is raised while IRQ0 is still pending is lost, because the
 * 8259 holds one request per line and a second edge while the first is
 * unserviced is not a second interrupt. The data area's count therefore
 * lags real time when the guest spends a long time with interrupts
 * disabled -- which is also true of the hardware, and is why the BIOS
 * count is a clock and not a stopwatch.
 */
#ifndef VM86_BIOS1A_H
#define VM86_BIOS1A_H

#include <stdint.h>

#include <vm86/cpu.h>

/* ------------------------------------------------------------------ */
/* The tick rate                                                       */
/* ------------------------------------------------------------------ */

/*
 * Ticks per millisecond, as an exact fraction: 1193182 / (65536 * 1000)
 * reduced by two.
 *
 * Exact rather than a rounded constant, because the accumulator below is
 * a remainder in this unit and a rounded one would drift. With the
 * fraction the error never exceeds one tick no matter how long the
 * machine runs -- the remainder carries, so nothing is thrown away.
 */
#define BIOS1A_TICKS_PER_MS_NUM 596591u
#define BIOS1A_TICKS_PER_MS_DEN 32768000u

/*
 * A day, in ticks: 0x1800B0, which is 1573040 -- eighteen and a bit ticks
 * a second, times 86400, rounded down, the same rounding the firmware
 * does. dos-refs.md section 6 is where that number comes from.
 *
 * The count is midnight to now, not a free-running counter that happens
 * to wrap somewhere, and the two are not the same thing: at midnight the
 * firmware *resets* the count to zero and raises the flag at 0040:0070,
 * rather than letting a thirty-two bit counter run over. A program that
 * treats the field as "ticks since the machine came up" is wrong about
 * every machine, not just this one.
 *
 * Note the size. The field is a double word and the day is 1573040, so
 * the top half is never anything but zero: the double word is there for
 * a machine whose count is allowed to grow, and the firmware never lets
 * it.
 */
#define BIOS1A_TICKS_PER_DAY 0x1800B0u

struct bios1a_state {
    /*
     * Milliseconds accumulated that have not yet made a whole tick, held
     * as a remainder in units of 1/BIOS1A_TICKS_PER_MS_DEN of a tick.
     *
     * Always strictly less than the denominator: `bios1a_advance()`
     * reduces it on every call and nothing else writes it, so it fits in
     * thirty-two bits with room to spare even though the multiplication
     * that feeds it does not.
     */
    uint32_t fraction;
};

/* ------------------------------------------------------------------ */
/* The host's clock                                                    */
/* ------------------------------------------------------------------ */

/*
 * Another `milliseconds` have gone by on the host's clock. Accumulate
 * them and return how many 18.2 Hz ticks that is worth.
 *
 * The host raises one IRQ0 per tick returned. It does not have to know
 * the rate, and it must not add to the count itself -- see the note above
 * about who owns 0040:006C.
 */
uint32_t bios1a_advance(struct bios1a_state *st, uint32_t milliseconds);

/* ------------------------------------------------------------------ */
/* INT 08h -- the timer interrupt                                      */
/* ------------------------------------------------------------------ */

/*
 * Add one to the count, roll it back to zero and raise the midnight flag
 * if that was the last tick of the day, and hand off to the user timer
 * hook at INT 1Ch.
 *
 * Register this against VM86_INT_TIMER. `ctx` is a `struct bios1a_state *`,
 * which this handler does not need -- the count lives in guest memory --
 * but the service signature is fixed and a host that registers one state
 * for both entry points should not have to special-case this one.
 */
void bios1a_irq(struct vm86_cpu *cpu, void *ctx);

/* ------------------------------------------------------------------ */
/* INT 1Ah -- what a program calls                                     */
/* ------------------------------------------------------------------ */

/*
 *   AH=00h  read the count, and clear the midnight flag on the way out
 *   AH=01h  set the count
 *
 * AH=02h and AH=04h -- read the real-time clock's time and date -- are
 * deliberately NOT implemented. They need CMOS hardware this machine does
 * not have, and returning zeroes would be worse than returning nothing:
 * a program reading 1980-01-01 00:00:00 has no way to tell that it is
 * looking at a machine with no clock, and one that reads the failure
 * convention can tell immediately. So those function numbers fall
 * through to the stub, which does nothing and returns, leaving AH as the
 * caller set it.
 *
 * `ctx` is a `struct bios1a_state *`.
 */
void bios1a_service(struct vm86_cpu *cpu, void *ctx);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Forget the fractional remainder. Does not touch guest memory. */
void bios1a_reset(struct bios1a_state *st);

/*
 * Reset the state and initialise the data-area fields this module owns:
 * the tick count and the midnight flag to zero.
 *
 * A machine that comes up at tick zero has been up for no time at all,
 * which is the only answer that does not invent a history -- and the
 * count at 0040:006C being zero while `INT 1Ah` answers correctly is
 * exactly the mismatch this call exists to prevent. On a real machine
 * the count would be whatever the POST left it at, which is nearer
 * midnight than not; zero is the same kind of answer and is at least
 * one a test can predict.
 */
void bios1a_init(struct vm86_cpu *cpu, struct bios1a_state *st);

#endif /* VM86_BIOS1A_H */
