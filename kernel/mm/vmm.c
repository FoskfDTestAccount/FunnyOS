#include <funnyos/vmm.h>
#include <funnyos/pmm.h>
#include <funnyos/bootinfo.h>
#include <funnyos/kprintf.h>

#include <stdbool.h>
#include <stdint.h>

#include <libk/string.h>

#define ENTRIES_PER_TABLE 512

/* A page table entry carries the physical address in bits 51:12. */
#define ENTRY_ADDR(entry) ((entry) & VMM_ADDR_MASK)

static uint64_t g_hhdm;
static uint64_t g_pml4_phys;
static bool     g_ready;
/* Page tables are physical; the HHDM is how we reach them. */
static inline uint64_t *phys_to_ptr(uint64_t phys)
{
    return (uint64_t *)(g_hhdm + phys);
}

static inline uint64_t ptr_to_phys(const void *p)
{
    return (uint64_t)p - g_hhdm;
}

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

static inline void tlb_invalidate(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* ------------------------------------------------------------------ */
/* Table traversal                                                     */
/* ------------------------------------------------------------------ */

/*
 * Fetch the table one level down, creating it when asked and possible.
 *
 * `level` is the level of `parent`: 3 = PML4, 2 = PDPT, 1 = PD.
 *
 * A large page in the way is broken up rather than reported as an error.
 * This matters more than it looks: the bootloader maps the HHDM with the
 * largest pages it can, and mapping a device register -- the local APIC
 * or an IO APIC -- lands squarely inside one of those. Without splitting,
 * every MMIO mapping in the kernel would depend on firmware happening not
 * to have covered that range with a 1 GiB page.
 *
 * Only a mapping request may split. A lookup or an unmap must not: it
 * would allocate frames as a side effect of a read-only query, and it
 * would silently convert one mapping into 512 for no reason.
 */
static uint64_t *descend(uint64_t *parent, uint64_t index, uint64_t flags,
                         bool create, int level)
{
    uint64_t entry = parent[index];

    if (entry & VMM_PRESENT) {
        if (!(entry & VMM_HUGE))
            return phys_to_ptr(ENTRY_ADDR(entry));

        if (!create)
            return NULL;

        if (level != 1 && level != 2) {
            /* Only a PD or a PDPT can hold a large-page entry. Anywhere
             * else the bit is a reserved-bit violation that the CPU would
             * fault on, so it is not a mapping we can work with. */
            kprintf("vmm: large page at level %d, which cannot carry one\n",
                    level);
            return NULL;
        }

        uint64_t frame = pmm_alloc_page();
        if (!frame) {
            kprintf("vmm: out of memory splitting a large page\n");
            return NULL;
        }

        uint64_t *table      = phys_to_ptr(frame);
        uint64_t  base       = ENTRY_ADDR(entry);
        uint64_t  child_size = (level == 2) ? (1ULL << 21) : PAGE_SIZE;
        uint64_t  child_flags;
        uint64_t  pat        = (entry & VMM_PAT_HUGE) ? 1u : 0u;

        /* Everything except the address, the PS bit and whatever else the
         * parent format defines differently. */
        uint64_t common = entry & ~VMM_ADDR_MASK & ~VMM_HUGE;

        if (level == 2) {
            /* 1 GiB -> 512 x 2 MiB. PAT sits in bit 12 in both formats and
             * the children are large pages themselves, so the flags carry
             * across unchanged. */
            child_flags = common | VMM_PRESENT | VMM_HUGE;
        } else {
            /* 2 MiB -> 512 x 4 KiB. PAT moves from bit 12 to bit 7, and
             * bit 7 is also where the parent format keeps PS -- so the
             * two have to be handled one at a time rather than masked off
             * together, which is exactly how this goes wrong. */
            child_flags = (common & ~VMM_PAT_HUGE) | VMM_PRESENT;
            if (pat)
                child_flags |= VMM_PAT_4K;
        }

        for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++)
            table[i] = (base + i * child_size) | child_flags;

        /*
         * The entry that used to name a large page now names a table, so
         * PS has to be clear and bit 12 stops being PAT and becomes an
         * ignored bit in a table reference. The permission bits carry
         * over from the large page, not from the caller's request: the
         * caller is describing the one leaf it is about to install, while
         * these bits describe the whole region being split.
         */
        parent[index] = frame |
                        (child_flags & (VMM_WRITABLE | VMM_USER |
                                        VMM_CACHE_DISABLE | VMM_WRITE_THROUGH)) |
                        VMM_PRESENT;

        tlb_invalidate(base);
        return table;
    }

    if (!create)
        return NULL;

    uint64_t frame = pmm_alloc_page();
    if (!frame)
        return NULL;

    /* Zeroing is not optional: an entry with stale bits in it produces a
     * fault far away from the code that failed to clear it. */
    memset(phys_to_ptr(frame), 0, PAGE_SIZE);

    /*
     * Writable and user permissions have to be granted at every level, not
     * just the leaf. A user mapping whose intermediate entry is
     * supervisor-only faults at the intermediate step, which is a
     * confusing way to learn this.
     */
    parent[index] = frame |
                    (flags & (VMM_WRITABLE | VMM_USER)) |
                    VMM_PRESENT;
    return phys_to_ptr(frame);
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

void vmm_init(void)
{
    g_hhdm = bootinfo_hhdm_offset();

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    g_pml4_phys = cr3 & VMM_ADDR_MASK;
    g_ready = true;
}

bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                     uint64_t flags)
{
    if (!g_ready)
        return false;
    if ((virt & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1)))
        return false;
    if (!pml4_phys)
        return false;

    uint64_t *pml4 = phys_to_ptr(pml4_phys);

    uint64_t *pdpt = descend(pml4, PML4_INDEX(virt), flags, true, 3);
    if (!pdpt)
        return false;

    uint64_t *pd = descend(pdpt, PDPT_INDEX(virt), flags, true, 2);
    if (!pd)
        return false;

    uint64_t *pt = descend(pd, PD_INDEX(virt), flags, true, 1);
    if (!pt)
        return false;

    pt[PT_INDEX(virt)] = ENTRY_ADDR(phys) | flags | VMM_PRESENT;

    /* Only the address space that is actually current has a TLB to
     * invalidate. Mapping into another one leaves the current TLB alone
     * and relies on that space's own CR3 load to flush it. */
    if (pml4_phys == g_pml4_phys)
        tlb_invalidate(virt);

    return true;
}

bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    return vmm_map_page_in(g_pml4_phys, virt, phys, flags);
}

bool vmm_unmap_page(uint64_t virt)
{
    if (!g_ready)
        return false;

    uint64_t *pml4 = phys_to_ptr(g_pml4_phys);

    uint64_t *pdpt = descend(pml4, PML4_INDEX(virt), 0, false, 3);
    if (!pdpt) return false;
    uint64_t *pd = descend(pdpt, PDPT_INDEX(virt), 0, false, 2);
    if (!pd) return false;
    uint64_t *pt = descend(pd, PD_INDEX(virt), 0, false, 1);
    if (!pt) return false;

    if (!(pt[PT_INDEX(virt)] & VMM_PRESENT))
        return false;

    pt[PT_INDEX(virt)] = 0;
    tlb_invalidate(virt);
    return true;
}

/*
 * Walk to the leaf entry for `virt` and hand back both the physical
 * address it resolves to and the entry itself.
 *
 * Returning the entry matters for the permission checks below: the
 * address alone cannot say whether a mapping is user-accessible, and a
 * caller that has to re-walk the tables to find out is a caller that will
 * eventually forget to.
 */
static uint64_t resolve_in(uint64_t pml4_phys, uint64_t virt,
                           uint64_t *leaf_entry_out,
                           bool *all_levels_user_out)
{
    if (!g_ready || !pml4_phys)
        return 0;

    bool all_user = true;

    uint64_t *pml4  = phys_to_ptr(pml4_phys);
    uint64_t  entry = pml4[PML4_INDEX(virt)];
    if (!(entry & VMM_PRESENT))
        return 0;
    all_user = all_user && (entry & VMM_USER) != 0;
    if (entry & VMM_HUGE)
        return 0;   /* PML4 has no large pages; the bit is reserved */

    uint64_t *pdpt = phys_to_ptr(ENTRY_ADDR(entry));
    entry = pdpt[PDPT_INDEX(virt)];
    if (!(entry & VMM_PRESENT))
        return 0;
    all_user = all_user && (entry & VMM_USER) != 0;
    if (entry & VMM_HUGE) {   /* 1 GiB page */
        if (leaf_entry_out)      *leaf_entry_out = entry;
        if (all_levels_user_out) *all_levels_user_out = all_user;
        return ENTRY_ADDR(entry) + (virt & 0x3FFFFFFF);
    }

    uint64_t *pd = phys_to_ptr(ENTRY_ADDR(entry));
    entry = pd[PD_INDEX(virt)];
    if (!(entry & VMM_PRESENT))
        return 0;
    all_user = all_user && (entry & VMM_USER) != 0;
    if (entry & VMM_HUGE) {   /* 2 MiB page */
        if (leaf_entry_out)      *leaf_entry_out = entry;
        if (all_levels_user_out) *all_levels_user_out = all_user;
        return ENTRY_ADDR(entry) + (virt & 0x1FFFFF);
    }

    uint64_t *pt = phys_to_ptr(ENTRY_ADDR(entry));
    entry = pt[PT_INDEX(virt)];
    if (!(entry & VMM_PRESENT))
        return 0;
    all_user = all_user && (entry & VMM_USER) != 0;

    if (leaf_entry_out)      *leaf_entry_out = entry;
    if (all_levels_user_out) *all_levels_user_out = all_user;

    return ENTRY_ADDR(entry) + (virt & 0xFFF);
}

uint64_t vmm_get_physical_in(uint64_t pml4_phys, uint64_t virt)
{
    return resolve_in(pml4_phys, virt, NULL, NULL);
}

uint64_t vmm_get_physical(uint64_t virt)
{
    return vmm_get_physical_in(g_pml4_phys, virt);
}

bool vmm_is_mapped(uint64_t virt)
{
    return vmm_get_physical(virt) != 0;
}

bool vmm_user_range_ok(uint64_t pml4_phys, uint64_t ptr, uint64_t len)
{
    /* The whole range has to be inside the half a process owns, which
     * rules out a kernel pointer being passed down and read on the
     * program's behalf. */
    if (ptr >= VMM_USER_LIMIT)
        return false;
    if (len > VMM_USER_LIMIT - ptr)
        return false;
    if (len == 0)
        return true;

    uint64_t first = ptr & ~(PAGE_SIZE - 1);
    uint64_t last  = (ptr + len - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t page = first; ; page += PAGE_SIZE) {
        bool all_user = false;

        if (!resolve_in(pml4_phys, page, NULL, &all_user))
            return false;

        /* The user bit has to be set at every level, not just the leaf:
         * an entry that is user-accessible under a supervisor-only
         * parent table faults rather than resolving. */
        if (!all_user)
            return false;

        if (page == last)
            break;
    }

    return true;
}

uint64_t vmm_pml4_physical(void)
{
    return g_pml4_phys;
}

/* ------------------------------------------------------------------ */
/* Address spaces                                                      */
/* ------------------------------------------------------------------ */

void vmm_switch_to(uint64_t pml4_phys)
{
    if (!pml4_phys)
        return;

    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t vmm_create_address_space(void)
{
    if (!g_ready)
        return 0;

    uint64_t frame = pmm_alloc_page();
    if (!frame)
        return 0;

    uint64_t *new_pml4 = phys_to_ptr(frame);
    uint64_t *cur_pml4 = phys_to_ptr(g_pml4_phys);

    memset(new_pml4, 0, PAGE_SIZE);

    /*
     * Share the kernel half by reference, not by copy.
     *
     * The tables these entries point at are the kernel's own, and every
     * address space must see the same ones: the HHDM so that a physical
     * address reached through the bootloader's offset keeps working, and
     * the kernel image so that an interrupt taken in Ring 3 lands in
     * mapped code.
     *
     * The lower half is left empty and belongs to the process.
     */
    for (int i = VMM_KERNEL_PML4_INDEX; i < ENTRIES_PER_TABLE; i++)
        new_pml4[i] = cur_pml4[i];

    return frame;
}

void vmm_destroy_address_space(uint64_t pml4_phys)
{
    if (!g_ready || !pml4_phys)
        return;

    uint64_t *pml4 = phys_to_ptr(pml4_phys);

    /*
     * Only the lower half is walked. The upper half points at the
     * kernel's tables, and freeing those would take the kernel down with
     * the process that happened to exit first.
     */
    for (int i = 0; i < VMM_KERNEL_PML4_INDEX; i++) {
        uint64_t entry = pml4[i];
        if (!(entry & VMM_PRESENT))
            continue;

        uint64_t *pdpt = phys_to_ptr(ENTRY_ADDR(entry));

        for (int j = 0; j < ENTRIES_PER_TABLE; j++) {
            uint64_t pdpt_e = pdpt[j];
            if (!(pdpt_e & VMM_PRESENT))
                continue;

            uint64_t *pd = phys_to_ptr(ENTRY_ADDR(pdpt_e));

            for (int k = 0; k < ENTRIES_PER_TABLE; k++) {
                uint64_t pd_e = pd[k];
                if (!(pd_e & VMM_PRESENT))
                    continue;

                /* A PD entry that maps a 2 MiB page points at memory, not
                 * at a table. Freeing it here would free a frame that is
                 * still in use somewhere; this kernel never creates such
                 * an entry in a process space, and the test is what makes
                 * that a property of the code rather than of its callers. */
                if (!(pd_e & VMM_HUGE))
                    pmm_free_page(ENTRY_ADDR(pd_e));
            }

            pmm_free_page(ENTRY_ADDR(pdpt_e));
        }

        pmm_free_page(ENTRY_ADDR(entry));
    }

    pmm_free_page(pml4_phys);
}
