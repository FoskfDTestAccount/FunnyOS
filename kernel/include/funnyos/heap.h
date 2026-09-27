/*
 * Kernel heap.
 *
 * A first-fit allocator over a doubly linked list of blocks, with splitting
 * on allocation and coalescing on free. Every block -- free or in use --
 * stays in the list, which is what makes coalescing possible: a freed block
 * can look at its neighbours directly instead of having to find them.
 *
 * Backing memory is fetched lazily. Each growth step allocates frames
 * individually from the frame allocator and maps them into a contiguous
 * virtual range, so the heap does not need physically contiguous memory --
 * only virtually contiguous, which is much easier to come by once the
 * machine has been running for a while.
 */
#ifndef FUNNYOS_HEAP_H
#define FUNNYOS_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bring up the heap. Requires the frame allocator and the virtual memory
 * manager to be ready. Returns false if the initial growth step fails. */
bool heap_init(void);

/* Allocate at least `size` bytes, 16-byte aligned. Returns NULL on
 * failure. */
void *kmalloc(size_t size);

/* Allocate and zero. */
void *kzalloc(size_t size);

/* Release an allocation. Passing NULL is allowed and does nothing. */
void kfree(void *ptr);

/* Statistics. */
size_t heap_capacity(void);   /* bytes mapped into the heap */
size_t heap_in_use(void);     /* bytes handed out */
size_t heap_free(void);       /* bytes available, excluding headers */
size_t heap_largest_free(void);

#endif /* FUNNYOS_HEAP_H */
