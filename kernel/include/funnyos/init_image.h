/*
 * The user program the kernel starts.
 *
 * Not a header anyone maintains by hand: the array and the length are
 * emitted by tools/bin2c.py from the flat binary the user build produces,
 * and the generated file is compiled into the kernel image.
 *
 * The indirection is deliberate. A user program is built with a different
 * code model, linked against a different linker script and laid out for a
 * different address, so the only thing the two sides can usefully share
 * is a blob of bytes and a count.
 */
#ifndef FUNNYOS_INIT_IMAGE_H
#define FUNNYOS_INIT_IMAGE_H

extern const unsigned char  funnyos_init_image[];
extern const unsigned long  funnyos_init_image_size;

#endif /* FUNNYOS_INIT_IMAGE_H */
