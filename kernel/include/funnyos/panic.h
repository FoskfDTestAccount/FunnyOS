/*
 * Kernel fatal error handling.
 *
 * At M0 there is no IDT, so any CPU exception triple-faults and reboots
 * the machine. panic() can therefore only be reached through software
 * detection of an unrecoverable condition (e.g. an unsupported boot
 * protocol revision). Once the IDT lands in M1, the exception handlers
 * will funnel into here as well.
 */
#ifndef FUNNYOS_PANIC_H
#define FUNNYOS_PANIC_H

/* Print diagnostics, then halt forever. Does not return. */
void panic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif /* FUNNYOS_PANIC_H */
