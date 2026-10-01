/*
 * What a machine says about itself.
 *
 * Two services and two words of the data area: how much memory there is,
 * and what equipment is attached. They are here rather than in one of the
 * service files because they belong to no device -- they are the
 * firmware's account of the machine, which is the same kind of statement
 * the vector table is.
 *
 * ---------------------------------------------------------------------
 * What this file writes, and the two exceptions it makes
 *
 * firmware.h says each field of the data area belongs to whichever
 * service owns it, and that nothing else may write across the area. That
 * rule holds for most of it, and this file is where it is broken -- twice,
 * on purpose, and for the same reason both times: the state has to exist
 * before any service has been asked for anything.
 *
 * The first is the two fields that describe the machine to itself: how
 * much memory there is and what is attached. Nothing maintains them and no
 * device is behind them; they are written once and read for the rest of
 * the machine's life. INT 11h and INT 12h below are readers.
 *
 * The second is the video half of the area. Those fields are the video
 * service's, and its reset() cannot publish them because it has no CPU to
 * write with. So for a while
 * nothing wrote them, and a machine that had been reset but not yet asked
 * a question reported a video mode of 0 and a page stride of 0 -- and a
 * program that reads the stride out of 0040:004C to work out where a page
 * is divides by zero. POST is what fills these on a real machine, which
 * makes this the firmware's job by the same argument as the first.
 *
 * It stays correct afterwards by itself: the video service publishes its
 * whole state on every call, so the first INT 10h overwrites every byte
 * here with the same values or with better ones.
 *
 * The tick count and the keyboard queue are *not* written here. Those have
 * owning services whose resets can fill them, and a copy would be a second
 * writer of the same bytes for no benefit.
 *
 * Neither of the paragraphs above lists which fields, and that is on
 * purpose. This header has been wrong about it twice, the same way both
 * times. First it listed the video fields among the ones this file leaves
 * alone, and when the code below started writing them the header was not
 * revisited -- so the file argued both ways, and a reader who trusted the
 * top of it concluded that a just-reset machine reported nothing about its
 * display. Then two more fields were added and the enumeration was not
 * extended, which is the same failure with the sign flipped.
 *
 * A list that has to be re-counted every time a field is added is a list
 * that will be wrong. A header that describes a different program from the
 * one underneath it is worse than no header, because it is read instead of
 * the code -- and this one is read by people asking what a machine looks
 * like before any service has been called, which is exactly the question
 * the list was getting wrong.
 *
 * ---------------------------------------------------------------------
 * Where these values come from
 *
 * Not from the 8086 manual, which says nothing about any of it: this is
 * the IBM PC, and the repository has neither the technical reference nor
 * Ralf Brown's list. So each value is spelled out bit by bit below rather
 * than written as a hex constant with a name, because a number nobody can
 * check is a number that stays wrong. Where a bit's meaning is not
 * certain it says so.
 */
#include <vm86/host.h>

#include <stddef.h>

#include <vm86/firmware.h>

/*
 * The equipment word, which INT 11h hands back in AX.
 *
 *   bit 0      a diskette drive is attached                      -> 1
 *   bit 1      an 8087 is present                                -> 0
 *   bits 2-3   motherboard RAM: 00 = 16K, 01 = 32K, 10 = 64K,
 *              11 = 64K or more                                 -> 11
 *   bits 4-5   initial video: 00 = none, 01 = 40x25 colour,
 *              10 = 80x25 colour, 11 = 80x25 monochrome           -> 10
 *   bits 6-7   diskette drives minus one                         -> 00
 *   bit 8      a DMA chip is present                             -> 0
 *   bits 9-11  number of serial ports                            -> 000
 *   bit 12     a game adapter is present                         -> 0
 *   bit 13     a serial printer is attached                      -> 0
 *   bits 14-15 number of parallel printers                       -> 00
 *
 * Bits 2-3 used to read 00 here, with a note claiming that was the
 * value an XT gives and that nothing read them back. Both halves of
 * that were wrong, and the second half was wrong in the way that
 * matters: INT 11h is exactly the channel that hands this word to a
 * program. A machine reporting a 16K motherboard while INT 12h reports
 * 640K of conventional memory is a machine contradicting itself in two
 * places a program can read, and the fix is one field.
 *
 * Beware of setting these bits by adding a number: 0x30 in the low byte
 * would move bits 4 and 5 as well, turning the display monochrome.
 */
#define VM86_EQUIPMENT_WORD 0x002Du

/*
 * Conventional memory, which INT 12h hands back in AX.
 *
 * 640 KiB is what a PC leaves below the video window, and it is what the
 * guest's own memory map has: the 1 MiB the chip can address, minus the
 * 384 KiB from 0xA0000 up that belongs to the display and the ROM area.
 *
 * This has to agree with what the loader actually puts in the guest's
 * first megabyte. A machine that claims more than it has is a machine
 * where a program allocates into nothing and the fault shows up somewhere
 * else entirely.
 */
#define VM86_CONVENTIONAL_KB 640u

/*
 * The data area lives at segment 0040, so an offset in it is that segment
 * shifted left four and added to. Spelled out once here rather than in
 * every accessor.
 */
static uint32_t bda(uint16_t offset)
{
    return ((uint32_t)VM86_BDA_SEGMENT << 4) + offset;
}

static void service_equipment(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;

    /*
     * Read out of the data area rather than out of the constant above, so
     * that the service and the memory a program can look at are the same
     * answer. A program that calls INT 11h and one that reads 0040:0010
     * directly have to agree; two sources would eventually not.
     */
    cpu->ax = vm86_mem_read16(cpu->mem, bda(VM86_BDA_EQUIPMENT));
}

static void service_memory_size(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;

    cpu->ax = vm86_mem_read16(cpu->mem, bda(VM86_BDA_MEMORY_KB));
}

void vm86_install_firmware(struct vm86_cpu *cpu)
{
    vm86_mem_write16(cpu->mem, bda(VM86_BDA_EQUIPMENT), VM86_EQUIPMENT_WORD);
    vm86_mem_write16(cpu->mem, bda(VM86_BDA_MEMORY_KB), VM86_CONVENTIONAL_KB);

    /*
     * The display, as a machine comes up: 80x25 colour text on page 0,
     * the cursor at home in the shape an AT gives it.
     *
     * This is the one place the who-writes-what rule is broken on
     * purpose, and the reason is that POST is a thing that happens.
     * Until now nothing wrote these four bytes, so a program that read
     * the video mode or the page stride out of the data area before
     * calling INT 10h got zeroes -- and the page stride being zero is
     * not a wrong answer, it is a division by zero in the program that
     * trusted it. Two separate reviews found this and neither could fix
     * it, because the field belongs to a service whose reset() has no
     * CPU to write with.
     *
     * It stays correct afterwards by itself: the video service publishes
     * its whole state on every call, so the first INT 10h overwrites
     * every byte here with the same values or with better ones. What
     * this buys is the window before anybody has called it, which is the
     * window a program reads the data area in.
     *
     * The values are repeated rather than taken from bios10.h on
     * purpose -- firmware does not depend on the video service, and a
     * test that wants a bare machine without one should still get a
     * machine that describes itself coherently.
     */
    vm86_mem_write8 (cpu->mem, bda(VM86_BDA_VIDEO_MODE), 3u);
    vm86_mem_write16(cpu->mem, bda(VM86_BDA_COLUMNS), VM86_TEXT_COLUMNS);
    vm86_mem_write16(cpu->mem, bda(VM86_BDA_PAGE_BYTES),
                     VM86_TEXT_PAGE_STRIDE);
    vm86_mem_write8 (cpu->mem, bda(VM86_BDA_ACTIVE_PAGE), 0u);
    vm86_mem_write16(cpu->mem, bda(VM86_BDA_CURSOR_SHAPE), 0x0607u);

    /* Rows is stored as one less than the count, and a machine that
     * leaves it at zero is a machine with one row. */
    vm86_mem_write8 (cpu->mem, bda(VM86_BDA_ROWS),
                     (uint8_t)(VM86_TEXT_ROWS - 1u));

    /* Modes do clear memory here, which is what bit 7 clear means. */
    vm86_mem_write8 (cpu->mem, bda(VM86_BDA_VIDEO_CONTROL), 0u);

    for (uint16_t page = 0; page < 8u; page++)
        vm86_mem_write16(cpu->mem, bda(VM86_BDA_CURSOR) + page * 2u, 0u);

    vm86_install_ivt(cpu);

    vm86_register_service(VM86_INT_EQUIPMENT,   service_equipment,   NULL);
    vm86_register_service(VM86_INT_MEMORY_SIZE, service_memory_size, NULL);
}
