/*
 * The user program the kernel starts.
 *
 * Not a header anyone maintains by hand: the array and the two lengths
 * are emitted by tools/bin2c.py from the flat binary the user build
 * produces, and the generated file is compiled into the kernel image.
 *
 * The indirection is deliberate. A user program is built with a different
 * code model, linked against a different linker script and laid out for a
 * different address, so the only thing the two sides can usefully share
 * is a blob of bytes, how long it is, and how much memory it wants.
 */
#ifndef FUNNYOS_INIT_IMAGE_H
#define FUNNYOS_INIT_IMAGE_H

/* The program's bytes, and how many of them there are. */
extern const unsigned char  funnyos_init_image[];
extern const unsigned long  funnyos_init_image_size;

/*
 * How much memory to build for it, which is the image length unless the
 * program declares a region of its own -- see _image_end in user/link.ld.
 * The kernel maps and zeroes this much; only the first
 * funnyos_init_image_size bytes are copied.
 */
extern const unsigned long  funnyos_init_image_mem_size;

#endif /* FUNNYOS_INIT_IMAGE_H */
