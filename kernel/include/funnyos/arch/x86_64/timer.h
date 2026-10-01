/*
 * System time base.
 *
 * The clock is the local APIC's built-in timer, running in periodic mode.
 * It needs no external device, it is per-CPU by construction (which
 * matters once there is more than one), and it is programmed entirely
 * through registers the kernel already has mapped.
 *
 * The one thing it cannot do is tell us its own frequency. The LAPIC
 * timer counts down from a divisor applied to a bus or core-crystal clock
 * whose rate varies by part -- typically 24 MHz, sometimes 100 MHz or the
 * full core clock. So the rate is measured rather than assumed: a
 * one-shot interval is timed against the 8254 PIT, whose 1.193182 MHz
 * crystal is the one frequency on a PC that actually is a fixed constant.
 *
 * The timestamp counter is calibrated in the same window, which yields a
 * free-running clock with sub-microsecond resolution alongside the tick.
 */
#ifndef FUNNYOS_ARCH_X86_64_TIMER_H
#define FUNNYOS_ARCH_X86_64_TIMER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Tick rate. 100 Hz is 10 ms of resolution, which is enough for a
 * scheduler and for measuring human-scale things, and cheap enough that
 * the handler's cost does not show up.
 *
 * It is deliberately not 1000 Hz: this kernel has no way to know yet
 * whether it is running under virtualisation where a 1 kHz tick is a
 * thousand VM exits a second, and nothing in M1 or M2 needs the
 * resolution.
 */
#define TIMER_HZ 100

/* The PIT's input frequency, exactly 1/3 of the NTSC colour burst and
 * the closest thing to a defined constant on a PC motherboard. */
#define PIT_FREQUENCY 1193182ULL

/* Bring up the LAPIC timer: calibrate it, program it for periodic ticks
 * on IRQ_VECTOR_TIMER, and register the handler. Leaves interrupts
 * globally disabled -- enabling them is the caller's decision. */
void timer_init(void);

bool timer_ready(void);

/* Ticks since timer_init(). Wraps after roughly 5.8 billion years. */
uint64_t timer_ticks(void);

/*
 * Ticks that arrived while a program was in Ring 3, and how many of those
 * landed somewhere other than that program's kernel stack.
 *
 * The second number is an invariant, not a statistic: it is zero on a
 * machine where every run gives rsp0 back the way it found it, and a
 * non-zero value means an interrupt pushed its frame onto a stack that
 * belonged to somebody else -- which after a nested run is a stack that
 * has since been freed. See process_kernel_stack_contains.
 */
uint64_t timer_ring3_ticks(void);
uint64_t timer_off_stack_ticks(void);

/* Milliseconds since timer_init(), derived from the tick count. */
uint64_t timer_millis(void);

/* Configured tick rate; always TIMER_HZ, but read it from here so the
 * value has one definition. */
uint32_t timer_hz(void);

/* Timestamp counter frequency in Hz, measured during calibration. Zero
 * when calibration failed. */
uint64_t timer_tsc_hz(void);

/* Measured rate of the LAPIC timer itself, after its divide register, in
 * counts per second. This is the number the tick period was computed
 * from, so it is what makes a wrong tick rate diagnosable. */
uint64_t timer_lapic_hz(void);

/* Free-running microseconds derived from the TSC. Has sub-tick
 * resolution but drifts against the tick count; use it for short
 * intervals, not for wall-clock time. */
uint64_t timer_tsc_micros(void);

/* Spin for at least `us` microseconds, measured on the TSC. A busy wait
 * -- it does not yield to the scheduler, because M1 has no scheduler. */
void timer_delay_micros(uint64_t us);

/*
 * Verify the clock by timing a known number of ticks against the TSC and
 * comparing the result with the rate that was programmed.
 *
 * Returns the measured rate in tenths of a hertz through `measured_hz_x10`
 * (zero when the check could not run) and reports whether it landed
 * within tolerance. A clock that ticks at the wrong rate is worse than no
 * clock at all, so this runs on every boot.
 */
bool timer_selftest(uint32_t *measured_hz_x10);

#endif /* FUNNYOS_ARCH_X86_64_TIMER_H */
