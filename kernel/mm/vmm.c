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
 * `level` is the level of `parent`: 3 = PML4, 2 = PDPT, 1 = PD. It matters
 * because a 2 MiB page can only be split once we are at the PD looking
 * down into a PT.
 */
static uint64_t *descend(uint64_t *parent, uint64_t index, uint64_t flags,
                         bool create, int level)
{
    uint64_t entry = parent[index];

    if (entry & VMM_PRESENT) {
        if (!(entry & VMM_HUGE))
            return phys_to_ptr(ENTRY_ADDR(entry));

        /*
         * A large page is already mapped here. Splitting a 2 MiB page into
         * a table of 4 KiB pages is mechanical and worth doing; splitting
         * a 1 GiB page would have to cascade two levels, and nothing in
         * M1 maps at that granularity in the ranges we touch.
         */
        if (level != 1 || !create) {
            kprintf("vmm: %s page in the way at level %d, cannot descend\n",
                    "large", level);
            return NULL;
        }

        uint64_t base  = ENTRY_ADDR(entry);            /* 2 MiB aligned */
        uint64_t pflags = entry & ~VMM_ADDR_MASK & ~VMM_HUGE;

        uint64_t frame = pmm_alloc_page();
        if (!frame) {
            kprintf("vmm: out of memory splitting a 2 MiB page\n");
            return NULL;
        }

        uint64_t *pt = phys_to_ptr(frame);
        memset(pt, 0, PAGE_SIZE);

        for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++)
            pt[i] = (base + i * PAGE_SIZE) | pflags | VMM_PRESENT;

        parent[index] = frame |
                        (pflags & (VMM_WRITABLE | VMM_USER)) |
                        VMM_PRESENT;
        tlb_invalidate(base);
        return pt;
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
