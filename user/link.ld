/*
 * User program layout.
 *
 * The program is linked at a fixed address because there is no loader to
 * relocate it: the kernel copies a flat image to PROCESS_CODE_BASE and
 * jumps to it, so everything the program contains has to already be
 * correct for that address at link time.
 *
 * _start is forced to offset zero. Nothing would break if it were not --
 * the kernel could be told an entry address -- but a flat image whose
 * first byte is the entry point is much easier to reason about when
 * something goes wrong.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    /*
     * .data and .bss sit adjacent, in this order, and that matters.
     *
     * The flat image is produced by objcopy with .bss marked loadable, so
     * it is emitted as real zero bytes and counted in the image size. Two
     * mechanisms are at work around uninitialised data and they do
     * different jobs:
     *
     *   This one puts ordinary uninitialised globals *inside* the image.
     *   The kernel therefore allocates pages for them and zeroes them,
     *   and a program's uninitialised data is guaranteed to be inside
     *   memory it owns. A NOBITS .bss would occupy no space in the file,
     *   fall outside the size the kernel is told, and fault on first
     *   touch.
     *
     *   The other, at _image_end below, covers memory the program will
     *   use whose contents are not in the image at all. The kernel maps
     *   and zeroes that part instead.
     *
     * They are complementary rather than redundant: the first is for
     * globals, which are small and whose zeroes cost little; the second
     * is for a region that must not be in the image, because the image
     * is compiled into the kernel as a C array at six characters a byte.
     */
    .data : ALIGN(8) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    .bss : ALIGN(8) {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    }

    /*
     * Memory the program will use but whose contents are not in the
     * image, and are not meant to be.
     *
     * NOLOAD is the whole point: the section contributes its size to the
     * address _image_end lands on, and nothing at all to the flat
     * binary. That difference -- mapped, but absent from the image -- is
     * exactly the pair of sizes the loader takes, and the reason it
     * takes two.
     *
     * The emulator that will run DOS programs declares its guest RAM
     * here. As an ordinary .bss array, those megabytes would be forced
     * into the image by the rule above, and from there into the kernel's
     * C source -- the design notes put the figure at about 96 MB of
     * source for 16 MB of memory.
     *
     * A program that declares nothing in this section gets an empty one
     * and an _image_end equal to the end of its .bss: no region, and the
     * loader is told the same size twice. That is the correct answer for
     * a program that does not want one.
     */
    .guestram (NOLOAD) : ALIGN(4096) {
        *(.guestram .guestram.*)
    }

    /*
     * Where the program's memory ends, as the linker sees it.
     *
     * The Makefile reads this out of the ELF and hands the kernel
     * `_image_end - 0x400000` as the size to build; what lies between
     * the end of the image and here is mapped and zeroed but never
     * copied.
     *
     * A linker symbol rather than a number written down in the kernel,
     * because a number would drift. Add an array to the program, forget
     * to update the constant, and the new array lands outside the
     * mapping and faults on its first write -- with the error pointing
     * at the new array, and not at the constant that is actually wrong.
     */
    _image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
