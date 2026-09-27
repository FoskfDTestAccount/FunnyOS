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

bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!g_ready)
        return false;
    if ((virt & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1)))
        return false;

    uint64_t *pml4 = phys_to_ptr(g_pml4_phys);

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
    tlb_invalidate(virt);
    return true;
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

uint64_t vmm_get_physical(uint64_t virt)
{
    if (!g_ready)
        return 0;

    uint64_t *pml4 = phys_to_ptr(g_pml4_phys);
    uint64_t entry = pml4[PML4_INDEX(virt)];
    if (!(entry & VMM_PRESENT) || (entry & VMM_HUGE))
        return 0;

    uint64_t *pdpt = phys_to_ptr(ENTRY_ADDR(entry));
    entry = pdpt[PDPT_INDEX(virt)];
    if (!(entry & VMM_PRESENT))
        return 0;
    if (entry & VMM_HUGE)   /* 1 GiB page */
        return ENTRY_ADDR(entry) + (virt & 0x3FFFFFFF);

    uint64_t *pd = phys_to_ptr(ENTRY_ADDR(entry));
    entry = pd[PD_INDEX(virt)];
    if (!(entry & VMM_PRESENT))
        return 0;
    if (entry & VMM_HUGE)   /* 2 MiB page */
        return ENTRY_ADDR(entry) + (virt & 0x1FFFFF);

    uint64_t *pt = phys_to_ptr(ENTRY_ADDR(entry));
    entry = pt[PT_INDEX(virt)];
    if (!(entry & VMM_PRESENT))
        return 0;

    return ENTRY_ADDR(entry) + (virt & 0xFFF);
}

bool vmm_is_mapped(uint64_t virt)
{
    return vmm_get_physical(virt) != 0;
}

uint64_t vmm_pml4_physical(void)
{
    return g_pml4_phys;
}
