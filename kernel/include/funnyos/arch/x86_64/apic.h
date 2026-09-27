/*
 * Local and I/O advanced programmable interrupt controllers.
 *
 * On x86-64 these are not optional. The 8259 PIC pair is a 16-bit-era
 * device whose vectors collide with the CPU exception range, and the
 * bootloader leaves the local APIC enabled with every LVT entry masked --
 * which is why an interrupt cannot arrive at all until something here
 * unmasks it.
 *
 * The two halves do different jobs:
 *
 *   - The LAPIC lives inside the CPU. It delivers the timer, the
 *     inter-processor interrupts, and -- importantly -- it is the
 *     component through which the IO APIC's messages reach the core. With
 *     the LAPIC disabled, nothing the IO APIC sends arrives.
 *
 *   - The IO APIC sits on the motherboard and routes the 24-or-so
 *     external interrupt lines to whichever LAPIC they belong to. It
 *     replaces the 8259 pair, but with per-line vector, trigger mode,
 *     polarity and destination instead of the PIC's fixed wiring.
 *
 * Both are memory-mapped, and both live in physical ranges that the boot
 * protocol does not put in the HHDM, so both are mapped explicitly and
 * uncached on the way in.
 */
#ifndef FUNNYOS_ARCH_X86_64_APIC_H
#define FUNNYOS_ARCH_X86_64_APIC_H

#include <stdbool.h>
#include <stdint.h>

/* --- Local APIC ---------------------------------------------------- */

/* Register offsets from the LAPIC MMIO base. */
#define LAPIC_ID          0x020u
#define LAPIC_VERSION     0x030u
#define LAPIC_TPR         0x080u
#define LAPIC_EOI         0x0B0u
#define LAPIC_LDR         0x0D0u
#define LAPIC_DFR         0x0E0u
#define LAPIC_SVR         0x0F0u
#define LAPIC_ESR         0x280u
#define LAPIC_LVT_CMCI    0x2F0u
#define LAPIC_ICR_LOW     0x300u
#define LAPIC_ICR_HIGH    0x310u
#define LAPIC_LVT_TIMER   0x320u
#define LAPIC_LVT_THERMAL 0x330u
#define LAPIC_LVT_PERF    0x340u
#define LAPIC_LVT_LINT0   0x350u
#define LAPIC_LVT_LINT1   0x360u
#define LAPIC_LVT_ERROR   0x370u
#define LAPIC_TIMER_INIT  0x380u
#define LAPIC_TIMER_CUR   0x390u
#define LAPIC_TIMER_DIV   0x3E0u

/* LVT entry bits. */
#define LAPIC_LVT_MASKED       (1u << 16)
#define LAPIC_LVT_LEVEL        (1u << 15)   /* LINT only: level triggered */
#define LAPIC_LVT_MODE_PERIODIC (1u << 17)
#define LAPIC_LVT_MODE_TSC_DEADLINE (2u << 17)

/* Spurious vector register: bit 8 enables the LAPIC in software, and the
 * low byte is the vector used for spurious interrupts. 0xFF is the
 * conventional choice -- high enough to sit above every device vector. */
#define LAPIC_SVR_ENABLE   (1u << 8)
#define LAPIC_SVR_VECTOR   0xFFu

/* Timer divide configuration values. The encoding is not monotonic: the
 * low two bits and bit 3 together select the divisor. */
#define LAPIC_DIV_1    0xBu
#define LAPIC_DIV_2    0x0u
#define LAPIC_DIV_4    0x1u
#define LAPIC_DIV_8    0x2u
#define LAPIC_DIV_16   0x3u
#define LAPIC_DIV_32   0x8u
#define LAPIC_DIV_64   0x9u
#define LAPIC_DIV_128  0xAu

/*
 * Bring up the local APIC: find it through the IA32_APIC_BASE MSR, map
 * it, enable it, raise its task priority to accept everything, and leave
 * every LVT entry masked. The mask is deliberate -- a vector is unmasked
 * by the driver that registers a handler for it, never before.
 */
void lapic_init(void);

bool     lapic_ready(void);
uint64_t lapic_base_physical(void);
uint64_t lapic_base_virtual(void);
uint32_t lapic_id(void);
uint32_t lapic_version(void);

uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t value);

/* Acknowledge the interrupt currently being serviced. Without this the
 * LAPIC considers the vector in-service and will not deliver another. */
void lapic_eoi(void);

/* --- I/O APIC ------------------------------------------------------ */

void ioapic_init(void);

bool     ioapic_ready(void);
uint32_t ioapic_id(void);
uint32_t ioapic_version(void);
uint64_t ioapic_address(void);

/* Number of redirection entries on the primary IO APIC, i.e. how many
 * interrupt lines it can route. */
uint32_t ioapic_entry_count(void);

/* Highest global system interrupt covered by any discovered IO APIC. */
uint32_t ioapic_max_gsi(void);

/* Mask or unmask one global system interrupt. */
void ioapic_mask_gsi(uint32_t gsi, bool masked);
void ioapic_mask_all(void);

/* Route a global system interrupt to a vector on this CPU. */
void ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool level_triggered,
                      bool active_low);

/*
 * Route one of the 16 ISA interrupts by its legacy number, applying any
 * MADT interrupt source override on the way. This is the interface a
 * device driver should use: the ISA IRQ number is what the hardware and
 * the documentation talk about, and firmware is entitled to have moved it
 * to a different global system interrupt.
 */
void ioapic_route_isa(unsigned isa_irq, uint8_t vector);

#endif /* FUNNYOS_ARCH_X86_64_APIC_H */
