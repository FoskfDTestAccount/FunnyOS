/*
 * The guest's memory.
 *
 * ---------------------------------------------------------------------
 * What this is not
 *
 * This is not the host memory allocator. It is the model of what the
 * emulated machine sees: a flat region of guest physical memory, reached
 * by linear addresses, with everything outside it behaving the way real
 * hardware does rather than the way a C array does.
 *
 * That last point is the whole reason this layer exists. On a real
 * machine, reading an address with nothing behind it returns 0xFF (the
 * floating bus) and writing one is silently dropped. Programs of the era
 * exploited both: probing for a device by reading a hole and checking for
 * 0xFF is a real idiom. An emulator that faults instead, or that returns
 * zero, breaks software that ran perfectly well on the hardware.
 *
 * ---------------------------------------------------------------------
 * Linear, not segmented
 *
 * These functions take a linear address -- segment base already added.
 * Working out the base from a segment register and an offset is a
 * separate concern and lives in decode.h, with the rest of the operand
 * addressing. Keeping them apart means a string instruction that walks a
 * buffer can add to an offset and call these, without redoing segment
 * arithmetic on every byte.
 */
#ifndef VM86_MEM_H
#define VM86_MEM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The A20 gate.
 *
 * The 8086 has twenty address lines, so a linear address wraps at 1 MiB.
 * The 286 and later have more, and the 21st line was gated to keep old
 * software from wrapping into the top of memory by accident; DOS programs
 * turn it on when they want the high memory area.
 *
 * It defaults to on here, because that is what a machine with HIMEM.SYS
 * loaded looks like, and that is how the programs this emulator is for
 * expect to find it. Emulators that leave it off exist and are wrong
 * about more things than this.
 */
struct vm86_mem {
    uint8_t *ram;      /* guest physical memory */
    uint32_t size;     /* how many bytes of it */
    bool     a20;      /* address line 20 gate */

    /* Counters for anything that fell outside. Not errors -- they are
     * how this machine is supposed to behave -- but a program that does
     * nothing else is worth being able to notice. */
    uint64_t unmapped_reads;
    uint64_t dropped_writes;
};

/* --- Construction --------------------------------------------------- */

/*
 * Point at memory the caller owns.
 *
 * There is deliberately no "allocate the guest's memory" function here.
 * Who provides the memory is a policy the emulator should make and this
 * layer should not: a host unit test mallocs a plain array, and the real
 * emulator maps a region into its own address space so that the guest's
 * writes land somewhere the kernel can protect. Neither of those is
 * something this file should have an opinion about, and a version that
 * called malloc() would not link on the freestanding side at all.
 *
 * Nothing is allocated and nothing is freed. The caller owns `ram` for as
 * long as the machine runs.
 */
void vm86_mem_attach(struct vm86_mem *mem, uint8_t *ram, uint32_t size);

/* Zero the guest's memory and reset the counters, leaving the region and
 * the A20 setting as they are. */
void vm86_mem_clear(struct vm86_mem *mem);

/* --- Access --------------------------------------------------------- */

/*
 * Read and write, by linear address.
 *
 * 16-bit accesses are performed as two 8-bit ones, so an access that
 * straddles the end of the region gets the same answer an address near
 * the boundary would -- half real, half floating bus -- rather than
 * reading past the end of the host's array.
 *
 * Every one of these is a candidate for inlining; callers in the opcode
 * handlers are all on the hot path.
 */
uint8_t  vm86_mem_read8 (struct vm86_mem *mem, uint32_t linear);
uint16_t vm86_mem_read16(struct vm86_mem *mem, uint32_t linear);
void     vm86_mem_write8 (struct vm86_mem *mem, uint32_t linear, uint8_t value);
void     vm86_mem_write16(struct vm86_mem *mem, uint32_t linear, uint16_t value);

/* Translate a linear address to an offset into `ram`, or UINT32_MAX when
 * it falls outside. Exposed because the display window and the ROM area
 * will want to intercept by address, and because tests want to look at
 * where something landed rather than at what it holds. */
#define VM86_MEM_UNMAPPED 0xFFFFFFFFu
uint32_t vm86_mem_offset(struct vm86_mem *mem, uint32_t linear);

/* --- The memory map, as far as it matters ---------------------------- */

/*
 * Guest memory is divided into regions that the machine treats
 * differently. Only the sizes matter here; the behaviour of each is the
 * emulator's business, not this layer's.
 */
#define VM86_ADDR_BASE_RAM    0x000000u   /* conventional memory */
#define VM86_ADDR_VGA_WINDOW  0x0A0000u   /* 0xA0000-0xBFFFF, display */
#define VM86_ADDR_VGA_SIZE    0x020000u
#define VM86_ADDR_ROM_AREA    0x0C0000u   /* 0xC0000-0xFFFFF, option ROMs */
#define VM86_ADDR_ROM_SIZE    0x040000u
#define VM86_ADDR_TOP         0x100000u   /* the 8086's 1 MiB ceiling */

#endif /* VM86_MEM_H */
