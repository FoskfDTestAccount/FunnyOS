/*
 * ACPI table discovery, limited to what the interrupt controller needs.
 *
 * The local APIC address and the IO APIC address are firm facts reported
 * by firmware, not constants. They happen to default to 0xFEE00000 and
 * 0xFEC00000 on essentially everything that has ever shipped, which is
 * exactly why guessing works right up until the machine where it does
 * not. Parsing the MADT costs a hundred lines and removes the guess.
 *
 * Only the RSDP -> XSDT/RSDT -> MADT path is implemented. Nothing here
 * touches AML, DSDT or any of the rest of ACPI; those tables stay
 * unparsed and unused.
 */
#ifndef FUNNYOS_ARCH_X86_64_ACPI_H
#define FUNNYOS_ARCH_X86_64_ACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An IO APIC as described by a MADT type-1 entry. */
struct acpi_ioapic_info {
    uint8_t  id;         /* hardware ID, also the destination field for it */
    uint32_t address;    /* physical MMIO base */
    uint32_t gsi_base;   /* first global system interrupt it owns */
};

/*
 * A MADT type-2 interrupt source override.
 *
 * The ISA IRQs and the IO APIC's global system interrupts are two
 * different numbering spaces that happen to start out identical. Firmware
 * may remap any of them: a machine can route the keyboard to GSI 8, or
 * swap the polarity on a level-triggered PCI line. A driver that indexes
 * the redirection table by ISA IRQ number works on most machines and
 * fails on the ones that took the override seriously.
 */
struct acpi_isa_override {
    bool     present;
    uint32_t gsi;
    bool     level_triggered;
    bool     active_low;
};

/* Locate the RSDP and parse the MADT out of it. Safe to call once the
 * frame allocator and page tables are up; does nothing if firmware
 * provides no usable ACPI tables. */
void acpi_init(void);

bool acpi_madt_found(void);

/* Physical address of the local APIC, or 0 when firmware did not say. */
uint64_t acpi_lapic_address(void);

size_t acpi_ioapic_count(void);
const struct acpi_ioapic_info *acpi_ioapic(size_t index);

/* Override for one of the 16 ISA IRQs. Never NULL; inspect ->present. */
const struct acpi_isa_override *acpi_isa_override(unsigned isa_irq);

/* The IO APIC owning a given global system interrupt, or NULL when the
 * GSI falls outside every one firmware described. */
const struct acpi_ioapic_info *acpi_ioapic_for_gsi(uint32_t gsi);

#endif /* FUNNYOS_ARCH_X86_64_ACPI_H */
