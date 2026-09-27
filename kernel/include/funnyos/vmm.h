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

/*
 * Bit 7 means two different things depending on the entry.
 *
 * In a page-directory or page-directory-pointer entry it is PS, the bit
 * that says "this entry maps a large page rather than pointing at the
 * next table down". In a leaf 4 KiB entry it is PAT, the cache type
 * selector. One name cannot honestly cover both, so it has two, and code
 * that converts between the two entry formats has to say which it means.
 */
#define VMM_HUGE           (1ULL << 7)   /* PS, in a PDE or PDPTE */
#define VMM_PAT_4K         (1ULL << 7)   /* PAT, in a 4 KiB PTE */

/* And bit 12 is PAT in a large-page entry -- 2 MiB or 1 GiB. */
#define VMM_PAT_HUGE       (1ULL << 12)

#define VMM_GLOBAL         (1ULL << 8)
#define VMM_NO_EXECUTE     (1ULL << 63)

/* Mask extracting the physical address from a page table entry. */
#define VMM_ADDR_MASK      0x000FFFFFFFFFF000ULL

/* Common combinations. */
#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITABLE)
#define VMM_USER_RW    (VMM_PRESENT | VMM_WRITABLE | VMM_USER)

/*
 * Top-level entries at and above this belong to the kernel and are shared
 * by every address space.
 *
 * The split falls out of the address layout rather than being chosen. The
 * HHDM sits at 0xffff800000000000, which is PML4 entry 256, and the
 * kernel image at 0xffffffff80000000, which is entry 511. The whole lower
 * canonical half -- every address a user program can name -- is entries 0
 * to 255.
 *
 * So a process address space is a private PML4 whose entries 256..511 are
 * copies of the kernel's, and whose entries 0..255 are its own. The
 * kernel stays mapped after a CR3 switch, which is what makes it possible
 * to take an interrupt while in Ring 3 at all.
 */
#define VMM_KERNEL_PML4_INDEX 256

/* The highest address user code can name: the end of the lower canonical
 * half, and therefore the end of the entries a process owns. */
#define VMM_USER_LIMIT 0x0000800000000000ULL

/* Record the current address space. Called after the frame allocator is
 * ready, because mapping needs frames for new tables. */
void vmm_init(void);

/* Map one 4 KiB page. `phys` must be page aligned. Existing mappings for
 * `virt` are replaced. Returns false if a table could not be allocated. */
bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* The same, into an address space other than the current one. */
bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                     uint64_t flags);

/* Remove a mapping and invalidate it. Returns false if it was not mapped. */
bool vmm_unmap_page(uint64_t virt);

/* Physical address backing `virt`, or 0 when unmapped. */
uint64_t vmm_get_physical(uint64_t virt);

/* The same, in an address space other than the current one. */
uint64_t vmm_get_physical_in(uint64_t pml4_phys, uint64_t virt);

/* Whether `virt` currently resolves. */
bool vmm_is_mapped(uint64_t virt);

/* Physical address of the top-level table (the value in CR3). */
uint64_t vmm_pml4_physical(void);

/*
 * A fresh address space: a private PML4 pre-populated with the kernel's
 * higher-half entries, and nothing else. Returns the physical address of
 * the new PML4, or 0 on failure.
 */
uint64_t vmm_create_address_space(void);

/*
 * Free the tables of an address space created above.
 *
 * The frames the process mapped -- its code and its stack -- are the
 * caller's to free, because only the caller knows which of them it
 * allocated and which might be shared. This frees the page tables and
 * nothing else, which keeps the two responsibilities from overlapping and
 * turning into a double free.
 */
void vmm_destroy_address_space(uint64_t pml4_phys);

/* Make an address space current. */
void vmm_switch_to(uint64_t pml4_phys);

/*
 * Whether [ptr, ptr + len) lies entirely in user space and is mapped with
 * user permission in the given address space.
 *
 * The kernel uses this before touching a pointer a user program supplied.
 * Checking first rather than catching a fault afterwards is enough here
 * because there is nothing that can change a process's mappings behind
 * its back -- no copy-on-write, no swapping, and no second thread. A
 * kernel that gained any of those would need a fault-recovery path
 * instead, because between the check and the access the mapping could
 * disappear.
 */
bool vmm_user_range_ok(uint64_t pml4_phys, uint64_t ptr, uint64_t len);

#endif /* FUNNYOS_VMM_H */
