/*
 * The programs the kernel can start.
 *
 * Today there is exactly one, and it is compiled into the kernel rather
 * than read from anywhere: the shell and the 8086 interpreter are the same
 * flat binary started in different modes (see the argument the kernel
 * hands to process_run). Naming it here, rather than growing a call that
 * means "start another copy of myself", is deliberate -- the shape a
 * program is *looked up by* is the part that is expensive to add after
 * anything depends on it, and a mounted DOS volume is where the next image
 * is going to come from.
 *
 * It is a function rather than a table of rows because the two lengths are
 * `const` variables emitted by tools/bin2c.py, and a `const` object is not
 * a constant expression in C -- so an initialiser cannot use them.
 */
#ifndef FUNNYOS_IMAGE_H
#define FUNNYOS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>

struct image_info {
    const void *data;         /* where the bytes are */
    size_t      size;         /* how many to copy */
    size_t      memory_size;  /* address space to build; see process_create */
};

/*
 * Look `name` up. Returns false when there is no such image, which is not
 * an error -- it is the answer, and the caller turns it into one.
 *
 * `out` is untouched on failure, so a caller that ignores the return value
 * does not get half an image.
 */
bool image_lookup(const char *name, struct image_info *out);

#endif /* FUNNYOS_IMAGE_H */
