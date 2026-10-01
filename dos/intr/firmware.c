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
 * These two are the exception to who-writes-what
 *
 * firmware.h says each field of the data area belongs to whichever
 * service owns it, and that nothing else may write across the area. These
 * two fields have no owning service: nothing maintains them, nothing
 * updates them, and no device is behind them. They are written once, at
 * power-on, and read for the rest of the machine's life.
 *
 * So this file writes exactly those two, and only those two. The mode
 * word, the cursor, the tick count and the keyboard queue are the video,
 * time and keyboard services' -- their resets fill them, and a copy here
 * would be a second writer of the same bytes for no benefit.
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
 *              11 = 64K or more
 *   bits 4-5   initial video: 00 = none, 01 = 40x25 colour,
 *              10 = 80x25 colour, 11 = 80x25 monochrome           -> 10
 *   bits 6-7   diskette drives minus one                         -> 00
 *   bit 8      a DMA chip is present                             -> 0
 *   bits 9-11  number of serial ports                            -> 000
 *   bit 12     a game adapter is present                         -> 0
 *   bit 13     a serial printer is attached                      -> 0
 *   bits 14-15 number of parallel printers                       -> 00
 *
 * The two bits that are not certain are bits 2-3. Their meaning is "how
 * much RAM is on the motherboard", which is a question this machine has
 * no answer to -- it has no motherboard to describe, and the memory it
 * actually has is what INT 12h reports out of the data area. They are
 * left at 00, which is the reading an XT gives, and nothing in this
 * machine reads them back.
 */
#define VM86_EQUIPMENT_WORD 0x0021u

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

    vm86_install_ivt(cpu);

    vm86_register_service(VM86_INT_EQUIPMENT,   service_equipment,   NULL);
    vm86_register_service(VM86_INT_MEMORY_SIZE, service_memory_size, NULL);
}
