#include <funnyos/pmm.h>
#include <funnyos/bootinfo.h>
#include <funnyos/kprintf.h>
#include <funnyos/panic.h>

#include <stddef.h>
#include <stdint.h>

#include <libk/string.h>

extern char __kernel_end[];

static uint8_t *g_bitmap;        /* virtual address, via the HHDM */
static uint64_t g_bitmap_phys;
static uint64_t g_bitmap_bytes;

static uint64_t g_total_pages;   /* frames covered by the bitmap */
static uint64_t g_free_pages;
static bool     g_ready;

/* ------------------------------------------------------------------ */
/* Bitmap primitives                                                   */
/* ------------------------------------------------------------------ */

/* A set bit means "in use". Starting from all-set and clearing the usable
 * regions means anything the memory map calls reserved -- or that the map
 * does not mention at all -- is reserved by construction, rather than by
 * remembering to reserve it. */
static inline void bit_set(uint64_t page)
{
    g_bitmap[page >> 3] |= (uint8_t)(1u << (page & 7));
}

static inline void bit_clear(uint64_t page)
{
    g_bitmap[page >> 3] &= (uint8_t)~(1u << (page & 7));
}

static inline bool bit_test(uint64_t page)
{
    return (g_bitmap[page >> 3] >> (page & 7)) & 1u;
}

/* Reserve or release a half-open range of *physical addresses*. */
static void mark_range(uint64_t base, uint64_t end, bool used)
{
    uint64_t first = PAGE_ALIGN_UP(base) / PAGE_SIZE;
    uint64_t last  = PAGE_ALIGN_DOWN(end) / PAGE_SIZE;

    for (uint64_t p = first; p < last && p < g_total_pages; p++) {
        if (used) {
            if (!bit_test(p)) {
                bit_set(p);
                g_free_pages--;
            }
        } else {
            if (bit_test(p)) {
                bit_clear(p);
                g_free_pages++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

void pmm_init(void)
{
    const uint64_t hhdm = bootinfo_hhdm_offset();
    const uint64_t count = bootinfo_memmap_entry_count();

    if (count == 0)
        panic("memory map is empty; cannot build a frame allocator");

    /* Size the bitmap from the highest usable address, so the machine's
     * memory decides the bitmap size rather than a compile-time guess. */
    uint64_t highest = 0;
    for (uint64_t i = 0; i < count; i++) {
        const struct limine_memmap_entry *e = bootinfo_memmap_entry(i);
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t end = e->base + e->length;
        if (end > highest)
            highest = end;
    }

    if (highest == 0)
        panic("no usable memory reported by the bootloader");

    g_total_pages  = highest / PAGE_SIZE;
    g_bitmap_bytes = (g_total_pages + 7) / 8;

    /*
     * Place the bitmap in the first usable region that can hold it. It
     * cannot live in a static array: the size depends on the machine.
     */
    for (uint64_t i = 0; i < count && g_bitmap_phys == 0; i++) {
        const struct limine_memmap_entry *e = bootinfo_memmap_entry(i);
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t base = PAGE_ALIGN_UP(e->base);
        uint64_t end  = PAGE_ALIGN_DOWN(e->base + e->length);
        if (end - base >= g_bitmap_bytes)
            g_bitmap_phys = base;
    }

    if (g_bitmap_phys == 0)
        panic("no usable region large enough for a %llu-byte frame bitmap",
              (unsigned long long)g_bitmap_bytes);

    g_bitmap = (uint8_t *)(hhdm + g_bitmap_phys);

    /* All reserved, then open up what the memory map actually offers. */
    memset(g_bitmap, 0xFF, g_bitmap_bytes);
    g_free_pages = 0;

    for (uint64_t i = 0; i < count; i++) {
        const struct limine_memmap_entry *e = bootinfo_memmap_entry(i);
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        mark_range(e->base, e->base + e->length, false);
    }

    /*
     * Re-reserve the things that live inside usable memory and must not be
     * handed out:
     *
     *   the bitmap itself
     *   the first MiB -- real-mode IVT, BDA and EBDA territory, and page 0
     *                     in particular, which doubles as this allocator's
     *                     "no memory" return value
     *   the kernel image
     *   bootloader-reclaimable memory, which holds the page tables we are
     *                     still running on
     */
    mark_range(g_bitmap_phys, g_bitmap_phys + g_bitmap_bytes, true);
    mark_range(0, 0x100000, true);

    uint64_t kphys = bootinfo_kernel_physical_base();
    uint64_t kvirt = bootinfo_kernel_virtual_base();
    if (kphys && kvirt)
        mark_range(kphys, kphys + (uint64_t)(__kernel_end - (char *)kvirt), true);

    for (uint64_t i = 0; i < count; i++) {
        const struct limine_memmap_entry *e = bootinfo_memmap_entry(i);
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            mark_range(e->base, e->base + e->length, true);
    }

    g_ready = true;
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

uint64_t pmm_alloc_page(void)
{
    if (!g_ready || g_free_pages == 0)
        return 0;

    for (uint64_t p = 1; p < g_total_pages; p++) {
        if (!bit_test(p)) {
            bit_set(p);
            g_free_pages--;
            return p * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_pages(uint64_t count)
{
    if (!g_ready || count == 0 || count > g_free_pages)
        return 0;

    if (count == 1)
        return pmm_alloc_page();

    /* First-fit over the bitmap. Slow, but correct and easy to reason
     * about; nothing in M1 allocates more than a handful of frames at a
     * time, and a free-list can replace this when that stops being true. */
    uint64_t run_start = 0;
    uint64_t run_len = 0;

    for (uint64_t p = 1; p < g_total_pages; p++) {
        if (bit_test(p)) {
            run_start = p + 1;
            run_len = 0;
            continue;
        }

        if (run_len == 0)
            run_start = p;
        run_len++;

        if (run_len == count) {
            for (uint64_t q = run_start; q < run_start + count; q++)
                bit_set(q);
            g_free_pages -= count;
            return run_start * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys)
{
    if (!g_ready || phys == 0)
        return;

    uint64_t p = phys / PAGE_SIZE;
    if (p >= g_total_pages) {
        kprintf("pmm: refusing to free out-of-range frame %p\n", (void *)phys);
        return;
    }
    if (!bit_test(p)) {
        /* Double free would corrupt the count and eventually hand the same
         * frame to two owners, which is much harder to debug than a
         * message. */
        kprintf("pmm: double free of frame %p\n", (void *)phys);
        return;
    }

    bit_clear(p);
    g_free_pages++;
}

void pmm_free_pages(uint64_t phys, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
        pmm_free_page(phys + i * PAGE_SIZE);
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

uint64_t pmm_total_pages(void) { return g_total_pages; }
uint64_t pmm_free_frame_count(void) { return g_free_pages; }
uint64_t pmm_used_pages(void)  { return g_total_pages - g_free_pages; }
bool     pmm_is_ready(void)    { return g_ready; }
