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
     * it is emitted as real zero bytes and counted in the image size. The
     * kernel therefore allocates pages for it and zeroes them, and a
     * program's uninitialised data is guaranteed to be inside memory it
     * owns. A NOBITS .bss would occupy no space in the file, fall outside
     * the size the kernel is told, and fault on first touch.
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

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
