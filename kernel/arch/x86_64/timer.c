#include <funnyos/arch/x86_64/timer.h>
#include <funnyos/arch/x86_64/apic.h>
#include <funnyos/arch/x86_64/cpu.h>
#include <funnyos/arch/x86_64/io.h>
#include <funnyos/arch/x86_64/irq.h>
#include <funnyos/kprintf.h>
#include <funnyos/process.h>

#include <stddef.h>

/* --- PIT access ---------------------------------------------------- */

#define PIT_CHANNEL2_DATA 0x42
#define PIT_COMMAND       0x43

/* Port B. Bit 0 gates PIT channel 2; bit 1 enables the speaker; bit 5 is
 * the state of channel 2's output pin, which is what makes this usable as
 * a polled delay without consuming an interrupt line. */
#define PORT_B            0x61
#define PORT_B_GATE2      (1u << 0)
#define PORT_B_SPEAKER    (1u << 1)
#define PORT_B_OUT2       (1u << 5)

/*
 * Reference interval for calibration, in PIT ticks: 50000 / 1.193182 MHz
 * = 41.9 ms. Long enough that the timer's own resolution and the cost of
 * reading the clock are both well under a tenth of a percent, short
 * enough not to be felt at boot.
 */
#define PIT_CALIBRATION_TICKS 50000u

/*
 * Bound on the polling loops below, in iterations.
 *
 * The expected number of iterations is the calibration interval divided
 * by the cost of one port read -- tens of thousands. Two million is a
 * margin of more than an order of magnitude, and it caps a broken PIT at
 * a fraction of a second instead of a hang. A hang here would be
 * particularly unpleasant: it happens before there is any clock to time
 * it out with.
 */
#define PIT_POLL_LIMIT 2000000u

/* --- State --------------------------------------------------------- */

static volatile uint64_t g_ticks;

/* Ticks that arrived while a program was in Ring 3, and how many of those
 * landed somewhere other than that program's kernel stack. See the check
 * in timer_tick. */
static uint64_t g_ring3_ticks;
static uint64_t g_off_stack_ticks;

static uint64_t g_lapic_hz;   /* measured, after the divide register */
static uint64_t g_tsc_hz;     /* measured */
static bool     g_ready;

/* --- Tick handler -------------------------------------------------- */

static void timer_tick(struct interrupt_frame *frame, void *ctx)
{
    (void)ctx;

    /*
     * One increment. Everything else -- scheduling, timeouts, sleeping --
     * is built on top of this number by later milestones, and every one
     * of them gets slower if the handler does more than this.
     */
    g_ticks++;

    /*
     * The one thing worth doing here besides counting, and it is here
     * because this is the only place it *can* be observed.
     *
     * The claim is that an interrupt taken while a program is in Ring 3
     * lands on that program's kernel stack. The CPU gets there through the
     * TSS, so the claim is really about rsp0 -- and rsp0 is exactly what a
     * nested run has to give back when it ends. A tick landing anywhere
     * else means some run left rsp0 pointing at a stack that is not the
     * running program's, which is what a finished child's freed stack
     * looks like.
     *
     * The address printed is the frame's, not rsp0's, and the difference
     * is the whole point: rsp0 says where the CPU was told to go, this
     * says where the frame actually ended up.
     *
     * Ring 3 only. A tick taken while the kernel is running is already on
     * whatever stack the kernel was using, and there is nothing to say
     * about it.
     */
    if ((frame->cs & 3u) == 3u) {
        g_ring3_ticks++;
        if (!process_kernel_stack_contains((uint64_t)frame))
            g_off_stack_ticks++;
    }
}

/* --- PIT-based reference interval ---------------------------------- */

/*
 * Busy-wait for `ticks` PIT channel-2 clocks.
 *
 * Channel 2 is the one the PC speaker uses, and it is the only PIT
 * channel whose output can be read back from software: bit 5 of port 0x61
 * reflects its OUT pin directly. That turns the 8254 into a stopwatch
 * that costs nothing but two port reads and steals no interrupt line.
 *
 * Mode 0 is "interrupt on terminal count": programming the mode drives
 * OUT low, and it rises again when the count reaches zero. So the wait is
 * a poll for that pin, bracketed by two bounded loops so that a PIT which
 * does not behave as documented degrades into a reported failure rather
 * than a boot that never finishes.
 */
static bool pit_delay(uint16_t ticks)
{
    /* Open the gate and keep the speaker silent. */
    uint8_t port_b = inb(PORT_B);
    outb(PORT_B, (uint8_t)((port_b & ~PORT_B_SPEAKER) | PORT_B_GATE2));

    /* Channel 2, lobyte/hibyte access, mode 0, binary counting. */
    outb(PIT_COMMAND, 0xB0);

    /* Wait for OUT to drop, which is what programming mode 0 does. If it
     * is already low this succeeds immediately. */
    uint32_t guard = 0;
    while (inb(PORT_B) & PORT_B_OUT2) {
        if (++guard > PIT_POLL_LIMIT)
            return false;
    }

    /* Load the count; counting starts on the second byte. */
    outb(PIT_CHANNEL2_DATA, (uint8_t)(ticks & 0xFF));
    outb(PIT_CHANNEL2_DATA, (uint8_t)(ticks >> 8));

    /* Wait for terminal count. */
    guard = 0;
    while (!(inb(PORT_B) & PORT_B_OUT2)) {
        if (++guard > PIT_POLL_LIMIT)
            return false;
    }

    return true;
}

/* --- Calibration --------------------------------------------------- */

static bool calibrate(void)
{
    /*
     * Run the LAPIC timer down from its maximum in one-shot mode, with
     * the LVT *unmasked*.
     *
     * Masking it looks tidier but is not safe to assume: whether a masked
     * LAPIC timer keeps counting is not something to build a measurement
     * on. Instead the timer runs for real and interrupts are simply
     * disabled, so nothing can be delivered. The countdown spans 0xFFFFFFFF
     * decrements -- minutes at any plausible rate -- so it cannot wrap and
     * fire within the 42 ms window.
     */
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_TIMER);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    uint64_t tsc_start = cpu_read_tsc();

    if (!pit_delay((uint16_t)PIT_CALIBRATION_TICKS)) {
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
        return false;
    }

    uint64_t tsc_end   = cpu_read_tsc();
    uint32_t remaining = lapic_read(LAPIC_TIMER_CUR);

    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

    /*
     * The decrement may have happened between reading the count and the
     * timestamp, so the elapsed LAPIC count can read one high. That is
     * 1 part in tens of thousands at these rates.
     */
    uint64_t lapic_elapsed = 0xFFFFFFFFull - remaining;

    g_tsc_hz   = (tsc_end - tsc_start) * PIT_FREQUENCY / PIT_CALIBRATION_TICKS;
    g_lapic_hz = lapic_elapsed          * PIT_FREQUENCY / PIT_CALIBRATION_TICKS;

    /*
     * Sanity-check the result. A plausible LAPIC timer after a divide of
     * 16 lands somewhere between 100 kHz and a few hundred MHz; anything
     * outside that means the PIT did not actually hold the interval it
     * was asked for, and a wrong clock is worse than none.
     */
    if (g_lapic_hz < 100000ull || g_lapic_hz > 1000000000ull)
        return false;

    return true;
}

/* --- Public interface ---------------------------------------------- */

bool timer_ready(void)     { return g_ready; }
uint64_t timer_ticks(void) { return g_ticks; }

uint64_t timer_ring3_ticks(void)     { return g_ring3_ticks; }
uint64_t timer_off_stack_ticks(void) { return g_off_stack_ticks; }
uint32_t timer_hz(void)    { return TIMER_HZ; }
uint64_t timer_tsc_hz(void) { return g_tsc_hz; }
uint64_t timer_lapic_hz(void) { return g_lapic_hz; }

uint64_t timer_millis(void)
{
    return g_ticks * 1000ull / TIMER_HZ;
}

uint64_t timer_tsc_micros(void)
{
    if (!g_tsc_hz)
        return 0;
    return cpu_read_tsc() * 1000000ull / g_tsc_hz;
}

void timer_delay_micros(uint64_t us)
{
    if (!g_tsc_hz) {
        /* No calibrated clock. Fall back to a spin whose duration is
         * unknown but bounded, rather than returning instantly and
         * letting a caller assume the delay happened. */
        for (volatile uint64_t i = 0; i < us * 1000ull; i++)
            ;
        return;
    }

    uint64_t target = cpu_read_tsc() + (us * g_tsc_hz) / 1000000ull;
    while (cpu_read_tsc() < target)
        cpu_pause();
}

void timer_init(void)
{
    if (!lapic_ready()) {
        kprintf("timer: local APIC unavailable, no tick source\n");
        return;
    }

    /* Calibration needs interrupts quiet: the timer runs unmasked for the
     * duration and must not be able to deliver anything. */
    interrupts_disable();

    if (!calibrate()) {
        kprintf("timer: calibration FAILED (PIT did not complete the "
                "reference interval)\n");
        return;
    }

    /*
     * Program the periodic tick.
     *
     * The divide register is left where calibration put it, so the
     * measured frequency still describes what the timer will do. If the
     * rate is so high that a single tick period overflows the 32-bit
     * initial count, step the divider up rather than programming a
     * truncated value -- a silently wrong tick rate is the one failure
     * this whole file exists to prevent.
     */
    uint64_t initial = g_lapic_hz / TIMER_HZ;
    while (initial > 0xFFFFFFFFull && g_lapic_hz < (1ull << 62)) {
        /* One more division by two: the encodings above 16 are 32, 64,
         * 128. Recalibrating would be more precise, but this path is
         * unreachable on real hardware. */
        lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV_128);
        g_lapic_hz /= 8;
        initial = g_lapic_hz / TIMER_HZ;
    }

    if (initial == 0) {
        kprintf("timer: measured rate is too low to produce a %u Hz tick\n",
                (unsigned)TIMER_HZ);
        return;
    }

    if (!irq_register(IRQ_VECTOR_TIMER, timer_tick, NULL)) {
        kprintf("timer: vector %u is already claimed\n",
                (unsigned)IRQ_VECTOR_TIMER);
        return;
    }

    /* Configure the LVT before loading the count: the initial count
     * register is what starts the timer, so it goes last. */
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_TIMER | LAPIC_LVT_MODE_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)initial);

    g_ready = true;
}

bool timer_selftest(uint32_t *measured_hz_x10)
{
    *measured_hz_x10 = 0;

    if (!g_ready || !g_tsc_hz)
        return false;

    /* A halted CPU with interrupts disabled never wakes up, and this test
     * waits for interrupts. Refuse rather than hang. */
    if (!interrupts_enabled())
        return false;

    const uint64_t sample_ticks = 20;   /* 200 ms at 100 Hz */

    /*
     * Wait for a tick boundary to start on. Starting at an arbitrary
     * phase would put up to one full tick period of error into a
     * measurement only twenty periods long.
     */
    uint64_t base = g_ticks;
    while (g_ticks == base)
        cpu_halt();

    uint64_t start_ticks = g_ticks;
    uint64_t start_tsc   = cpu_read_tsc();

    /* A ceiling, so a tick that never arrives fails instead of hanging. */
    uint64_t deadline = start_tsc + 2ull * g_tsc_hz;

    while (g_ticks - start_ticks < sample_ticks) {
        cpu_halt();
        if (cpu_read_tsc() > deadline)
            return false;
    }

    uint64_t elapsed_us = (cpu_read_tsc() - start_tsc) * 1000000ull / g_tsc_hz;
    if (elapsed_us == 0)
        return false;

    /* Tenths of a hertz, to avoid floating point in the kernel. */
    uint64_t hz_x10 = sample_ticks * 10000000ull / elapsed_us;
    *measured_hz_x10 = (uint32_t)hz_x10;

    /*
     * Five percent. The measurement itself is far better than that -- the
     * boundary wait and the halt-wakeup latency are both microseconds --
     * so the tolerance is there to absorb scheduler noise and the
     * virtualisation the tests run under, not measurement error.
     */
    uint64_t want = (uint64_t)TIMER_HZ * 10;
    uint64_t low  = want - want / 20;
    uint64_t high = want + want / 20;

    return hz_x10 >= low && hz_x10 <= high;
}
