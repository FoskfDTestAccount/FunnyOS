/*
 * Physical page frame allocator.
 *
 * Tracks 4 KiB physical frames in a bitmap, one bit per frame, built from
 * the memory map the bootloader supplies. The bitmap is placed inside the
 * first usable region rather than in a static array, so its size follows
 * the machine instead of being capped at whatever was guessed at compile
 * time.
 *
 * Frames are handed out by physical address. Callers reach them through
 * the HHDM (see bootinfo_hhdm_offset()), which maps all of physical memory
 * at a fixed virtual offset.
 */
#ifndef FUNNYOS_PMM_H
#define FUNNYOS_PMM_H

#include <stdbool.h>
#include <stdint.h>

#define PAGE_SIZE       4096ULL
#define PAGE_SHIFT      12

/* Page containing the given address. */
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(addr)   (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* Build the frame bitmap from the bootloader's memory map.
 * Must be called before any allocation. */
void pmm_init(void);

/* Allocate one frame. Returns its physical address, or 0 when memory is
 * exhausted (address 0 is never a valid frame: it is left reserved). */
uint64_t pmm_alloc_page(void);

/* Allocate `count` physically contiguous frames. Returns the physical
 * address of the first, or 0 on failure. */
uint64_t pmm_alloc_pages(uint64_t count);

/* Return a frame to the pool. */
void pmm_free_page(uint64_t phys);

/* Return a contiguous run to the pool. */
void pmm_free_pages(uint64_t phys, uint64_t count);

/* Statistics. */
uint64_t pmm_total_pages(void);
uint64_t pmm_used_pages(void);
uint64_t pmm_free_frame_count(void);

/* Whether the allocator is initialised. */
bool pmm_is_ready(void);

#endif /* FUNNYOS_PMM_H */
