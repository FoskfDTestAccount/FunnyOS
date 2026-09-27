/*
 * Virtual memory: x86-64 4-level page table manipulation.
 *
 * Page tables are physical structures reached through the HHDM, so every
 * table pointer here is a physical address that gets converted on access.
 *
 * Note on the top-level table: this does not build a private PML4. The
 * kernel keeps using the one the bootloader built, and maps into it. That
 * table lives in bootloader-reclaimable memory, which pmm_init() reserves
 * so it cannot be handed out. A private PML4 becomes necessary when
 * address spaces have to be per-process, which is M2's problem.
 */
#ifndef FUNNYOS_VMM_H
#define FUNNYOS_VMM_H

#include <stdbool.h>
#include <stdint.h>

/* Page table entry flags. */
#define VMM_PRESENT        (1ULL << 0)
#define VMM_WRITABLE       (1ULL << 1)
#define VMM_USER           (1ULL << 2)
#define VMM_WRITE_THROUGH  (1ULL << 3)
#define VMM_CACHE_DISABLE  (1ULL << 4)
#define VMM_ACCESSED       (1ULL << 5)
#define VMM_DIRTY          (1ULL << 6)
#define VMM_HUGE           (1ULL << 7)
#define VMM_GLOBAL         (1ULL << 8)
#define VMM_NO_EXECUTE     (1ULL << 63)

/* Mask extracting the physical address from a page table entry. */
#define VMM_ADDR_MASK      0x000FFFFFFFFFF000ULL

/* Common combinations. */
#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITABLE)
#define VMM_USER_RW    (VMM_PRESENT | VMM_WRITABLE | VMM_USER)

/* Record the current address space. Called after the frame allocator is
 * ready, because mapping needs frames for new tables. */
void vmm_init(void);

/* Map one 4 KiB page. `phys` must be page aligned. Existing mappings for
 * `virt` are replaced. Returns false if a table could not be allocated. */
bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Remove a mapping and invalidate it. Returns false if it was not mapped. */
bool vmm_unmap_page(uint64_t virt);

/* Physical address backing `virt`, or 0 when unmapped. */
uint64_t vmm_get_physical(uint64_t virt);

/* Whether `virt` currently resolves. */
bool vmm_is_mapped(uint64_t virt);

/* Physical address of the top-level table (the value in CR3). */
uint64_t vmm_pml4_physical(void);

#endif /* FUNNYOS_VMM_H */
