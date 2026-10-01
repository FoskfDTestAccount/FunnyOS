#include "bios1a.h"

#include <vm86/firmware.h>
#include <vm86/host.h>
#include <vm86/mem.h>

/*
 * The data area is reached by linear address; these offsets are relative
 * to segment 0040. The same three lines exist in bios16.c, and that is
 * deliberate rather than an oversight: each module is one translation
 * unit with its own static helper, and the alternative -- a shared header
 * -- would mean editing one of the frozen interface headers, which this
 * milestone does not allow. It is three lines and it cannot disagree with
 * itself.
 */
static uint32_t bda_linear(uint16_t offset)
{
    return ((uint32_t)VM86_BDA_SEGMENT << 4) + offset;
}

static uint32_t tick_count(struct vm86_mem *mem)
{
    uint32_t at = bda_linear(VM86_BDA_TICK_COUNT);

    return (uint32_t)vm86_mem_read16(mem, at)
         | ((uint32_t)vm86_mem_read16(mem, at + 2) << 16);
}

static void set_tick_count(struct vm86_mem *mem, uint32_t value)
{
    uint32_t at = bda_linear(VM86_BDA_TICK_COUNT);

    vm86_mem_write16(mem, at, (uint16_t)(value & 0xFFFFu));
    vm86_mem_write16(mem, at + 2, (uint16_t)(value >> 16));
}

/* ------------------------------------------------------------------ */
/* The host's clock                                                    */
/* ------------------------------------------------------------------ */

/*
 * Milliseconds in, ticks out, with nothing thrown away.
 *
 * The naive version of this -- divide the milliseconds by 54.925 and
 * truncate -- loses up to a tick on every call, and a host that polls
 * every few milliseconds loses it every few milliseconds. Over an hour
 * that is a clock a program can measure and complain about.
 *
 * So the remainder is kept: the accumulator is in units of
 * 1/32768000 of a tick, which is the exact fraction 18.2065 ticks per
 * second written out, and a slice of time that is not worth a whole tick
 * today is worth one tomorrow. The error is therefore always less than
 * one tick and never grows -- the only rounding is the final truncation,
 * and the count it truncates is the *total* count, not this call's share
 * of it.
 */
uint32_t bios1a_advance(struct bios1a_state *st, uint32_t milliseconds)
{
    uint64_t total = st->fraction
                   + (uint64_t)milliseconds * BIOS1A_TICKS_PER_MS_NUM;

    uint32_t ticks = (uint32_t)(total / BIOS1A_TICKS_PER_MS_DEN);

    st->fraction = (uint32_t)(total % BIOS1A_TICKS_PER_MS_DEN);

    return ticks;
}

/* ------------------------------------------------------------------ */
/* INT 08h                                                             */
/* ------------------------------------------------------------------ */

void bios1a_irq(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;   /* the count is in guest memory, not in the state */

    struct vm86_mem *mem = cpu->mem;
    uint32_t ticks = tick_count(mem) + 1u;

    /*
     * Midnight. The count runs from zero to 0x1800B0 and starts again,
     * and the flag at 0040:0070 goes up to say so.
     *
     * Note what this is not. It is not a thirty-two bit counter that
     * happens to be reset -- the field is a double word and the day's
     * worth of ticks fits in twenty-one bits, so the top half is dead
     * weight on a real machine. It is not "detected and reported"
     * either: the count really does go back to zero, and a program that
     * did not read the flag has lost the day boundary for good.
     *
     * The flag is set, never incremented or counted, so a second
     * midnight arriving before anyone reads the first leaves no trace.
     * That is the firmware's behaviour and a real defect in it -- DOS
     * dates have been known to stall because of it -- and it is
     * reproduced here rather than repaired, because a program written
     * against the real machine expects exactly this.
     */
    if (ticks >= BIOS1A_TICKS_PER_DAY) {
        uint32_t at = bda_linear(VM86_BDA_TICK_ROLLOVER);
        uint8_t  flags = vm86_mem_read8(mem, at);

        ticks = 0;

        vm86_mem_write8(mem, at, (uint8_t)(flags | 0x01u));
    }

    set_tick_count(mem, ticks);

    /*
     * Hand off to the user timer hook.
     *
     * This looks like it belongs in the run loop and does not. INT 1Ch is
     * a vector the firmware calls from the end of its own timer handler,
     * and firmware has done it that way since the first PC: the hook runs
     * *inside* the timer interrupt, with the interrupt still in service,
     * so a program that installs one does not have to chain anything --
     * it just writes its address into the table and the firmware calls
     * it. Leaving the call out and letting a host dispatch to 1Ch would
     * work for the first program to hook it and stop working for the
     * second.
     *
     * The frame this pushes is unwound by the guest's own IRET: the 1Ch
     * stub's if nothing is hooked, or whatever a program hooked, which
     * returns here either way. That is the whole of the chaining
     * mechanism and it is three lines long.
     */
    vm86_interrupt(cpu, VM86_INT_USER_TIMER);
}

/* ------------------------------------------------------------------ */
/* INT 1Ah                                                             */
/* ------------------------------------------------------------------ */

void bios1a_service(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;

    struct vm86_mem *mem = cpu->mem;

    switch (cpu->ah) {
    case 0x00: {
        uint32_t at = bda_linear(VM86_BDA_TICK_ROLLOVER);
        uint8_t  flags = vm86_mem_read8(mem, at);

        /*
         * Read the flag and clear it before reading the count. A program
         * that spans midnight compares the count with the flag, and it
         * can only do that if the two are consistent -- so the flag has
         * to describe the count that is about to be handed back, and
         * clearing it is what makes the next read mean "since this one".
         *
         * There is no window to race in here: the only thing that could
         * increment the count mid-service is the timer interrupt, and a
         * service runs with the interpreter inside it.
         */
        vm86_mem_write8(mem, at, 0);

        uint32_t ticks = tick_count(mem);

        cpu->al = flags;
        cpu->cx = (uint16_t)(ticks >> 16);
        cpu->dx = (uint16_t)(ticks & 0xFFFFu);
        return;
    }

    case 0x01:
        /*
         * Set the count, and reset the midnight flag with it.
         *
         * The flag means "midnight has passed since the count was last
         * read or written", and a program that has just written the
         * count has read it in every sense that matters -- leaving the
         * flag up would tell it that a day boundary happened at some
         * unknown point in a count it is currently defining. Ralf
         * Brown's entry for this function says so in as many words
         * ("this call resets the midnight-passed flag"); dos-refs.md
         * section 6 does not mention it, so it is the one behaviour here
         * that rests on a source outside that file.
         */
        vm86_mem_write8(mem, bda_linear(VM86_BDA_TICK_ROLLOVER), 0);
        set_tick_count(mem, ((uint32_t)cpu->cx << 16) | (uint32_t)cpu->dx);
        return;

    default:
        /*
         * 02h and 04h land here, and so does anything else. They need
         * CMOS hardware this machine does not have, and the honest
         * failure is to do nothing: AH keeps the function number the
         * caller asked for, which is how a program detects that the call
         * was not implemented. Answering with zeroes would look like
         * 1980-01-01 00:00:00 and a program would believe it.
         */
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void bios1a_reset(struct bios1a_state *st)
{
    st->fraction = 0;
}

void bios1a_init(struct vm86_cpu *cpu, struct bios1a_state *st)
{
    bios1a_reset(st);

    set_tick_count(cpu->mem, 0);
    vm86_mem_write8(cpu->mem, bda_linear(VM86_BDA_TICK_ROLLOVER), 0);
}
