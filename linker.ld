/*
 * FunnyOS kernel linker script.
 *
 * Constraints imposed by the Limine boot protocol:
 *   - The kernel must be linked into the higher half, at or above
 *     0xffffffff80000000.
 *   - A .limine_requests section must exist, wrapped by the start/end
 *     markers.
 *   - Each section gets its own program header so the bootloader can map
 *     them with the correct MMU permissions.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    /* Start of the higher half. Limine requires the kernel to sit within
     * the topmost 2 GiB of the address space. */
    . = 0xffffffff80000000;

    /* Boot request area. KEEP stops --gc-sections from discarding it. */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        *(.got .got.*)
    } :data

    /* .bss shares the data program header but is marked NOBITS, so it
     * occupies no space in the file. */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* These cause problems on some hosts; drop them. */
    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }

    /* Link-time symbol used by the kernel for self-reporting. */
    . = ALIGN(4096);
    __kernel_end = .;
}
