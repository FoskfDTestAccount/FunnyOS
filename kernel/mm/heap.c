#include <funnyos/heap.h>
#include <funnyos/pmm.h>
#include <funnyos/vmm.h>
#include <funnyos/kprintf.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libk/string.h>

/* Virtual range reserved for the heap, well clear of the kernel image. */
#define HEAP_BASE  0xffffffffa0000000ULL
#define HEAP_LIMIT 0xffffffffb0000000ULL   /* 256 MiB of address space */

#define HEAP_GROW  (256 * 1024)            /* bytes per growth step */
#define MIN_SPLIT  64                      /* never leave a smaller remainder */

#define BLOCK_MAGIC 0x48454150u            /* "HEAP" */
#define FLAG_FREE   0x1u

/*
 * 8 + 8 + 8 + 4 + 4 = 32 bytes, so as long as the heap base is 16-byte
 * aligned every usable area stays 16-byte aligned, which is what the
 * calling convention wants for anything that might hold a vector or a
 * long double.
 */
struct heap_block {
    uint64_t size;                 /* usable bytes after the header */
    struct heap_block *next;
    struct heap_block *prev;
    uint32_t magic;
    uint32_t flags;
};

static struct heap_block *g_head;
static uint64_t g_brk;             /* next unmapped virtual address */
static size_t   g_capacity;
static size_t   g_in_use;

static inline void *block_data(struct heap_block *b)
{
    return (void *)(b + 1);
}

static inline struct heap_block *block_of(void *ptr)
{
    return (struct heap_block *)ptr - 1;
}

/* ------------------------------------------------------------------ */
/* List management                                                     */
/* ------------------------------------------------------------------ */

static void list_append(struct heap_block *b)
{
    b->next = NULL;

    if (!g_head) {
        b->prev = NULL;
        g_head = b;
        return;
    }

    struct heap_block *tail = g_head;
    while (tail->next)
        tail = tail->next;
    tail->next = b;
    b->prev = tail;
}

/*
 * Note there is no list_remove. Blocks never leave the list -- freeing one
 * only clears its free flag. Staying linked is what lets a freed block
 * look directly at its neighbours and coalesce with them.
 */

/* ------------------------------------------------------------------ */
/* Growth                                                              */
/* ------------------------------------------------------------------ */

static bool heap_grow(size_t min_bytes)
{
    size_t want = HEAP_GROW;
    if (want < min_bytes + sizeof(struct heap_block))
        want = min_bytes + sizeof(struct heap_block);
    want = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (g_brk + want > HEAP_LIMIT) {
        kprintf("heap: virtual range exhausted (%llu bytes mapped)\n",
                (unsigned long long)g_capacity);
        return false;
    }

    const uint64_t start = g_brk;
    size_t mapped = 0;

    for (size_t off = 0; off < want; off += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_page();
        if (!frame) {
            kprintf("heap: out of physical memory after %llu bytes\n",
                    (unsigned long long)mapped);
            goto rollback;
        }
        if (!vmm_map_page(start + off, frame, VMM_KERNEL_RW)) {
            pmm_free_page(frame);
            kprintf("heap: failed to map at %p\n", (void *)(start + off));
            goto rollback;
        }
        mapped += PAGE_SIZE;
    }

    g_brk += want;
    g_capacity += want;

    struct heap_block *b = (struct heap_block *)start;
    b->size  = want - sizeof(struct heap_block);
    b->magic = BLOCK_MAGIC;
    b->flags = FLAG_FREE;
    list_append(b);

    return true;

rollback:
    /* Leave the address space exactly as it was found. A partial growth
     * that stayed mapped would leak the frames and fragment the range. */
    for (size_t off = 0; off < mapped; off += PAGE_SIZE) {
        uint64_t phys = vmm_get_physical(start + off);
        if (phys) {
            vmm_unmap_page(start + off);
            pmm_free_page(phys);
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

void *kmalloc(size_t size)
{
    if (size == 0 || !g_head)
        return NULL;

    size = (size + 15) & ~(size_t)15;   /* keep 16-byte alignment */

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct heap_block *b = g_head; b; b = b->next) {
            if (!(b->flags & FLAG_FREE) || b->size < size)
                continue;

            /*
             * Split only when the remainder can hold a header plus
             * something useful. Splitting down to slivers produces a list
             * full of blocks nobody can ever satisfy.
             */
            if (b->size >= size + sizeof(struct heap_block) + MIN_SPLIT) {
                struct heap_block *rest =
                    (struct heap_block *)((char *)block_data(b) + size);

                rest->size  = b->size - size - sizeof(struct heap_block);
                rest->magic = BLOCK_MAGIC;
                rest->flags = FLAG_FREE;

                rest->next = b->next;
                rest->prev = b;
                if (b->next)
                    b->next->prev = rest;
                b->next = rest;

                b->size = size;
            }

            b->flags &= ~FLAG_FREE;
            g_in_use += b->size;
            memset(block_data(b), 0, b->size);   /* see the header comment */
            return block_data(b);
        }

        /* Nothing fit. Try once more with a fresh chunk before giving up. */
        if (attempt == 0 && !heap_grow(size))
            return NULL;
    }

    return NULL;
}

void *kzalloc(size_t size)
{
    return kmalloc(size);
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct heap_block *b = block_of(ptr);

    if (b->magic != BLOCK_MAGIC) {
        kprintf("heap: kfree(%p) does not point at a heap block\n", ptr);
        return;
    }
    if (b->flags & FLAG_FREE) {
        /* Double free would hand the same memory to two owners later on,
         * which surfaces as corruption far from the cause. Better to
         * complain here. */
        kprintf("heap: double free of %p\n", ptr);
        return;
    }

    b->flags |= FLAG_FREE;
    g_in_use -= b->size;

    /* Coalesce forwards. The list is address ordered, so an adjacent
     * block is exactly one whose header follows this block's data. */
    if (b->next && (b->next->flags & FLAG_FREE) &&
        (char *)b->next == (char *)block_data(b) + b->size) {
        struct heap_block *n = b->next;
        b->size += sizeof(struct heap_block) + n->size;
        b->next = n->next;
        if (n->next)
            n->next->prev = b;
        n->magic = 0;
    }

    /* Coalesce backwards. */
    if (b->prev && (b->prev->flags & FLAG_FREE) &&
        (char *)b == (char *)block_data(b->prev) + b->prev->size) {
        struct heap_block *p = b->prev;
        p->size += sizeof(struct heap_block) + b->size;
        p->next = b->next;
        if (b->next)
            b->next->prev = p;
        b->magic = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

size_t heap_capacity(void) { return g_capacity; }
size_t heap_in_use(void)   { return g_in_use; }

size_t heap_free(void)
{
    size_t total = 0;
    for (struct heap_block *b = g_head; b; b = b->next)
        if (b->flags & FLAG_FREE)
            total += b->size;
    return total;
}

size_t heap_largest_free(void)
{
    size_t best = 0;
    for (struct heap_block *b = g_head; b; b = b->next)
        if ((b->flags & FLAG_FREE) && b->size > best)
            best = b->size;
    return best;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

bool heap_init(void)
{
    g_head = NULL;
    g_brk = HEAP_BASE;
    g_capacity = 0;
    g_in_use = 0;

    return heap_grow(0);
}
