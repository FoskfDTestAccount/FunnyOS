/*
 * Unified access to boot information.
 *
 * The rest of the kernel reads Limine-provided data through this
 * interface instead of touching limine.h request structures directly.
 * Swapping the bootloader therefore only requires reimplementing this
 * one file.
 */
#ifndef FUNNYOS_BOOTINFO_H
#define FUNNYOS_BOOTINFO_H

#include <stdbool.h>
#include <stdint.h>

#include <limine.h>

/* Whether the bootloader supports the base revision we requested.
 * Must be checked early during startup. */
bool bootinfo_base_revision_ok(void);

/* Bootloader name and version. */
const char *bootinfo_loader_name(void);
const char *bootinfo_loader_version(void);

/* Firmware type; one of the LIMINE_FIRMWARE_TYPE_* constants. */
uint64_t bootinfo_firmware_type(void);

/* Offset of the Higher Half Direct Map. The virtual address of physical
 * address `phy` is `bootinfo_hhdm_offset() + phy`. */
uint64_t bootinfo_hhdm_offset(void);

/* Kernel command line supplied by the bootloader, or "" when absent.
 * Used to gate opt-in behaviour such as fault injection in tests. */
const char *bootinfo_cmdline(void);

/* Physical memory map. */
uint64_t bootinfo_memmap_entry_count(void);
const struct limine_memmap_entry *bootinfo_memmap_entry(uint64_t index);

/* Total size in bytes of all memory map entries of the given type. */
uint64_t bootinfo_memory_total_by_type(uint64_t type);

/* Preferred framebuffer, or NULL when none is available. */
struct limine_framebuffer *bootinfo_framebuffer(void);

/*
 * Root System Description Pointer, the entry point to every ACPI table.
 * Returns NULL when firmware did not supply one.
 *
 * ACPI 1.0 systems have no RDSP at all, so a NULL here is not necessarily
 * an error -- it means the machine predates the tables or the firmware
 * chose not to expose them.
 */
void *bootinfo_rsdp(void);

/* Physical base address the kernel image was loaded at. */
uint64_t bootinfo_kernel_physical_base(void);

/* Virtual base address of the kernel image. */
uint64_t bootinfo_kernel_virtual_base(void);

#endif /* FUNNYOS_BOOTINFO_H */
