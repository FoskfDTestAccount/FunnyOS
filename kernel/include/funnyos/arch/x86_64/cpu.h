/*
 * Model-specific registers, CPUID and the timestamp counter.
 *
 * These are the reads that have no portable equivalent and no state of
 * their own -- a register number in, a value out -- so they live in a
 * header rather than a translation unit.
 */
#ifndef FUNNYOS_ARCH_X86_64_CPU_H
#define FUNNYOS_ARCH_X86_64_CPU_H

#include <stdint.h>

/* --- Model-specific registers -------------------------------------- */

#define MSR_IA32_APIC_BASE 0x1Bu
#define MSR_IA32_EFER      0xC0000080u

static inline uint64_t cpu_read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     : /* no outputs */
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* --- Timestamp counter --------------------------------------------- */

/*
 * The TSC counts at a constant rate on any CPU that reports an invariant
 * TSC, which is every x86-64 part of the last decade. It is the finest
 * clock available without an HPET, and it is free to read from Ring 0.
 *
 * It is not used as the system tick -- the LAPIC timer is -- but it is
 * what makes the tick rate measurable rather than merely assumed, and
 * later it will serve for sub-millisecond delays.
 */
static inline uint64_t cpu_read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* --- CPUID ---------------------------------------------------------- */

struct cpu_cpuid_result {
    uint32_t eax, ebx, ecx, edx;
};

static inline struct cpu_cpuid_result cpu_cpuid(uint32_t leaf, uint32_t subleaf)
{
    struct cpu_cpuid_result r;
    __asm__ volatile("cpuid"
                     : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(subleaf));
    return r;
}

/* --- Control registers --------------------------------------------- */

static inline uint64_t cpu_read_cr0(void)
{
    uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline uint64_t cpu_read_cr2(void)
{
    uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static inline uint64_t cpu_read_cr3(void)
{
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline uint64_t cpu_read_cr4(void)
{
    uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}

static inline void cpu_write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}
static inline void cpu_write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

/* --- Hints ---------------------------------------------------------- */

/* Tell the CPU this is a spin-wait, so it can yield execution resources
 * and stop speculating into a loop that is about to be left. */
static inline void cpu_pause(void)
{
    __asm__ volatile("pause");
}

#endif /* FUNNYOS_ARCH_X86_64_CPU_H */
